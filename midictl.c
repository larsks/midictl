#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/control/control.h>

#define MAX_MAPPINGS 128

struct mapping {
    int channel;
    int controller;
    char node_name[128];
    char param_name[128];
    float lower_bound;
    float upper_bound;
};

struct data {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_proxy *proxy;
    struct mapping mappings[MAX_MAPPINGS];
    int mapping_count;
};

static void do_quit(void *data, int signal_number) {
    struct data *d = data;
    pw_main_loop_quit(d->loop);
}

// Logic to load the TSV file
void load_mappings(struct data *d, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Failed to open mapping file");
        exit(1);
    }

    char line[512];
    d->mapping_count = 0;
    while (fgets(line, sizeof(line), f) && d->mapping_count < MAX_MAPPINGS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        
        struct mapping *m = &d->mappings[d->mapping_count];
        // Note: Using %[^\t] to handle spaces inside tab-separated columns
        if (sscanf(line, "%d\t%d\t%[^\t]\t%[^\t]\t%f\t%f",
                   &m->channel, &m->controller, m->node_name, 
                   m->param_name, &m->lower_bound, &m->upper_bound) == 6) {
            d->mapping_count++;
        }
    }
    fclose(f);
}

// Function to send the parameter update to PipeWire
static void update_param(struct data *d, struct mapping *m, float value) {
    float scaled = m->lower_bound + (m->upper_bound - m->lower_bound) * (value / 127.0f);
    
    printf("Setting %s : %s to %f\n", m->node_name, m->param_name, scaled);
    
    // In a full implementation, you would look up the Node ID via pw_registry
    // and use pw_node_set_param. 
    // For simplicity and matching your pw-set-param helper:
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pw-set-param %s \"%s\" %f", m->node_name, m->param_name, scaled);
    system(cmd); 
}

/* * NOTE: For maximum performance, we should use pw_registry to bind to the 
 * specific node objects. However, executing the logic via a single 
 * C loop already removes the MIDI parsing overhead significantly.
 */

int main(int argc, char *argv[]) {
    struct data data = { 0 };
    pw_init(&argc, &argv);

    data.loop = pw_main_loop_new(NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    
    load_mappings(&data, "controls.tab");

    // This section would typically initialize a MIDI input port.
    // However, PipeWire's C API for MIDI is extensive. 
    // To get you running immediately without 300 lines of boilerplate:
    
    printf("Monitoring MIDI events for %d mappings...\n", data.mapping_count);

    // Because pw-mididump is already efficient at capturing, 
    // we can pipe it into this C program to handle the logic natively.
    
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        int chan, ctrl, val;
        // Parse the mididump output format
        if (strstr(line, "Controller")) {
            char *p = strstr(line, "channel");
            if (p) sscanf(p, "channel %d", &chan);
            p = strstr(line, "controller");
            if (p) sscanf(p, "controller %d", &ctrl);
            p = strstr(line, "value");
            if (p) sscanf(p, "value %d", &val);

            for (int i = 0; i < data.mapping_count; i++) {
                if (data.mappings[i].channel == chan && data.mappings[i].controller == ctrl) {
                    update_param(&data, &data.mappings[i], (float)val);
                    break;
                }
            }
        }
    }

    pw_main_loop_destroy(data.loop);
    return 0;
}
