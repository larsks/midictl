/* midi-control.c
 *
 * Listens for MIDI controller events via PipeWire and maps them to PipeWire
 * node parameters according to a SPA-JSON configuration file.
 *
 * Config format (controls.json):
 * [
 *   { "channel": 1, "control": 23, "node": "gain.input",
 *     "param": "gain:Gain 1", "min": 0.0, "max": 10.0 },
 *   ...
 * ]
 *
 * Build:
 *   gcc $(pkg-config --cflags --libs libpipewire-0.3) -o midi-control
 * midi-control.c
 */

#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>

/* --------------------------------------------------------------------------
 * Compile-time limits
 * -------------------------------------------------------------------------- */
#define MAX_MAPPINGS 256
#define MAX_NODES 256
#define MAX_PARAM_NAME 128
#define MAX_NODE_NAME 128
#define JSON_BUF_SIZE 65536

/* --------------------------------------------------------------------------
 * Logging helpers
 * -------------------------------------------------------------------------- */
static bool verbose = false;

#define log_verbose(fmt, ...)                                                  \
  do {                                                                         \
    if (verbose)                                                               \
      fprintf(stderr, "[verbose] " fmt "\n", ##__VA_ARGS__);                   \
  } while (0)

#define log_info(fmt, ...) fprintf(stderr, "[info]    " fmt "\n", ##__VA_ARGS__)
#define log_warn(fmt, ...) fprintf(stderr, "[warn]    " fmt "\n", ##__VA_ARGS__)
#define log_error(fmt, ...)                                                    \
  fprintf(stderr, "[error]   " fmt "\n", ##__VA_ARGS__)

/* --------------------------------------------------------------------------
 * Data structures
 * -------------------------------------------------------------------------- */

/* One entry from the JSON config file */
struct mapping {
  int channel;                   /* MIDI channel (1-based) */
  int control;                   /* MIDI CC number */
  char node_name[MAX_NODE_NAME]; /* PipeWire node.name to target */
  char param[MAX_PARAM_NAME];    /* Parameter name, e.g. "nr:bypass" */
  double min;                    /* Scaled output minimum */
  double max;                    /* Scaled output maximum */

  /* Resolved at runtime */
  uint32_t node_id;        /* pw_node id, or SPA_ID_INVALID */
  bool is_param;           /* true  -> Props { params: ["name", val] }
                              false -> Props { name: val }           */
  bool prop_info_resolved; /* have we queried PropInfo yet? */
};

/* Carries a parameter update from the RT process thread to the main loop */
struct param_update {
  struct pw_proxy *proxy;
  char param[MAX_PARAM_NAME];
  float value;
  bool is_param;
};

/* One tracked PipeWire node */
struct node_info {
  uint32_t id;
  char name[MAX_NODE_NAME];
  struct pw_proxy *proxy;
  struct spa_hook proxy_listener;
  struct spa_hook node_listener;
  struct data *data; /* back-pointer */
};

/* Top-level application state */
struct data {
  struct pw_main_loop *loop;
  struct pw_loop *pw_loop; /* pw_main_loop_get_loop(loop), cached */
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;

  struct pw_registry *registry;
  struct spa_hook registry_listener;

  /* MIDI source port proxy */
  struct pw_proxy *midi_port_proxy;
  struct spa_hook midi_port_listener;
  uint32_t midi_port_id; /* registry id of our MIDI port */
  uint32_t midi_node_id; /* node that owns the port */

  /* Port we create to receive MIDI */
  struct pw_node *capture_node;

  /* Tracked graph nodes */
  struct node_info nodes[MAX_NODES];
  int n_nodes;

  /* Mappings loaded from config */
  struct mapping mappings[MAX_MAPPINGS];
  int n_mappings;

  /* Pending core sync */
  int pending_seq;
};

/* --------------------------------------------------------------------------
 * Config parsing
 * -------------------------------------------------------------------------- */

static int load_config(const char *path, struct mapping *mappings,
                       int max_mappings) {
  FILE *f = fopen(path, "r");
  if (!f) {
    log_error("Cannot open config file '%s': %s", path, strerror(errno));
    return -1;
  }

  char *buf = malloc(JSON_BUF_SIZE);
  if (!buf) {
    fclose(f);
    return -1;
  }

  size_t n = fread(buf, 1, JSON_BUF_SIZE - 1, f);
  fclose(f);
  buf[n] = '\0';

  struct spa_json root, arr, obj;
  spa_json_init(&root, buf, n);

  /* spa_json_enter_container does spa_json_next + spa_json_enter atomically.
   * Never call spa_json_next on a container token and then spa_json_enter
   * separately — by then iter->cur has moved past the opening bracket and
   * enter would start the child at the wrong position. */
  if (spa_json_enter_container(&root, &arr, '[') <= 0) {
    log_error("Config must be a JSON array");
    free(buf);
    return -1;
  }

  int count = 0;
  while (count < max_mappings) {
    /* Enter each object element of the array */
    if (spa_json_enter_container(&arr, &obj, '{') <= 0)
      break;

    struct mapping *m = &mappings[count];
    memset(m, 0, sizeof(*m));
    m->node_id = SPA_ID_INVALID;

    char key[64];
    bool have_channel = false, have_control = false, have_node = false,
         have_param = false, have_min = false, have_max = false;

    while (spa_json_get_string(&obj, key, sizeof(key)) > 0) {
      if (strcmp(key, "channel") == 0) {
        if (spa_json_get_int(&obj, &m->channel) > 0)
          have_channel = true;
      } else if (strcmp(key, "control") == 0) {
        if (spa_json_get_int(&obj, &m->control) > 0)
          have_control = true;
      } else if (strcmp(key, "node") == 0) {
        if (spa_json_get_string(&obj, m->node_name, sizeof(m->node_name)) > 0)
          have_node = true;
      } else if (strcmp(key, "param") == 0) {
        if (spa_json_get_string(&obj, m->param, sizeof(m->param)) > 0)
          have_param = true;
      } else if (strcmp(key, "min") == 0) {
        float fv;
        if (spa_json_get_float(&obj, &fv) > 0) {
          m->min = (double)fv;
          have_min = true;
        }
      } else if (strcmp(key, "max") == 0) {
        float fv;
        if (spa_json_get_float(&obj, &fv) > 0) {
          m->max = (double)fv;
          have_max = true;
        }
      } else {
        /* skip unknown key's value */
        const char *val;
        spa_json_next(&obj, &val);
      }
    }

    if (!have_channel || !have_control || !have_node || !have_param ||
        !have_min || !have_max) {
      log_warn("Skipping incomplete mapping entry (need channel, control, "
               "node, param, min, max)");
      continue;
    }

    log_verbose("Loaded mapping: ch=%d cc=%d node='%s' param='%s' "
                "min=%.4f max=%.4f",
                m->channel, m->control, m->node_name, m->param, m->min, m->max);
    count++;
  }

  free(buf);
  log_info("Loaded %d mapping(s) from '%s'", count, path);
  return count;
}

/* --------------------------------------------------------------------------
 * Value scaling
 * -------------------------------------------------------------------------- */
static double scale_midi(int midi_val, double out_min, double out_max) {
  return out_min + (out_max - out_min) * (midi_val / 127.0);
}

/* --------------------------------------------------------------------------
 * Parameter setting via libpipewire
 * -------------------------------------------------------------------------- */

/* Build and send a Props pod to set a single parameter on a node proxy.
 *
 * If is_param == true:   Props { params: [ "name", value ] }
 * If is_param == false:  Props { name: value }
 *
 * Must be called from the main loop thread. The RT process callback uses
 * do_set_node_param_invoke via pw_loop_invoke to get here safely.
 */
static void set_node_param(struct pw_proxy *proxy, const char *param_name,
                           float value, bool is_param) {
  uint8_t buf[512];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
  struct spa_pod_frame f[2];
  struct spa_pod *pod;

  /* Props { params: [ "name", <Float> ] }
   *
   * Must use a Struct pod (heterogeneous), not an Array pod (homogeneous),
   * because the params value is a sequence of alternating string/float pairs.
   */
  spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props,
                              SPA_PARAM_Props);
  spa_pod_builder_prop(&b, SPA_PROP_params, 0);
  spa_pod_builder_push_struct(&b, &f[1]);
  spa_pod_builder_string(&b, param_name);
  spa_pod_builder_float(&b, value);
  spa_pod_builder_pop(&b, &f[1]);
  pod = spa_pod_builder_pop(&b, &f[0]);

  int res = pw_node_set_param((struct pw_node *)proxy, SPA_PARAM_Props, 0, pod);
  /*
  if (verbose) {
      fprintf(stderr, "[verbose] pw_node_set_param: res=%d (%s), pod size=%u\n",
              res, spa_strerror(res), SPA_POD_SIZE(pod));
      uint8_t *p = (uint8_t *)pod;
      fprintf(stderr, "[verbose] pod bytes:");
      for (uint32_t i = 0; i < SPA_POD_SIZE(pod) && i < 64; i++)
          fprintf(stderr, " %02x", p[i]);
      fprintf(stderr, "\n");
  }
  */
}

/* pw_loop_invoke callback — runs in the main loop thread */
static int do_set_node_param_invoke(struct spa_loop *loop, bool async,
                                    uint32_t seq, const void *data, size_t size,
                                    void *user_data) {
  (void)loop;
  (void)async;
  (void)seq;
  (void)size;
  const struct param_update *u = data;
  /*
  log_verbose("invoke: setting param='%s' value=%.4f on proxy %p",
              u->param, u->value, (void *)u->proxy);
  */
  set_node_param(u->proxy, u->param, u->value, u->is_param);
  return 0;
}

/* Called from the RT process thread — dispatches to main loop */
static void schedule_node_param(struct pw_loop *loop, struct pw_proxy *proxy,
                                const char *param_name, float value,
                                bool is_param) {
  struct param_update u;
  u.proxy = proxy;
  u.value = value;
  u.is_param = is_param;
  snprintf(u.param, sizeof(u.param), "%s", param_name);
  int res = pw_loop_invoke(loop, do_set_node_param_invoke, SPA_ID_INVALID, &u,
                           sizeof(u), false, NULL);
  if (res < 0)
    fprintf(stderr, "[error]   pw_loop_invoke failed: %s\n", spa_strerror(res));
}

/* --------------------------------------------------------------------------
 * PropInfo querying: resolve is_param for a mapping
 * -------------------------------------------------------------------------- */

/* Temporary state used while enumerating PropInfo */
struct propinfo_query {
  const char *param_name;
  bool found;
  bool is_param;
};

static void on_node_param(void *data, int seq, uint32_t id, uint32_t index,
                          uint32_t next, const struct spa_pod *param) {
  /* We only care about PropInfo pods */
  if (id != SPA_PARAM_PropInfo || param == NULL)
    return;

  struct node_info *ni = data;
  struct data *d = ni->data;

  /* Iterate over all unresolved mappings for this node */
  for (int i = 0; i < d->n_mappings; i++) {
    struct mapping *m = &d->mappings[i];
    if (m->node_id != ni->id)
      continue;
    if (m->prop_info_resolved)
      continue;

    /* Parse the PropInfo pod to extract name and params flag */
    const char *name = NULL;
    bool is_par = false;

    struct spa_pod_prop *prop;
    SPA_POD_OBJECT_FOREACH((struct spa_pod_object *)param, prop) {
      if (prop->key == SPA_PROP_INFO_name) {
        if (spa_pod_get_string(&prop->value, &name) < 0)
          name = NULL;
      } else if (prop->key == SPA_PROP_INFO_params) {
        bool v = false;
        if (spa_pod_get_bool(&prop->value, &v) >= 0)
          is_par = v;
      }
    }

    if (name && strcmp(name, m->param) == 0) {
      m->is_param = is_par;
      m->prop_info_resolved = true;
      log_verbose("Resolved PropInfo for '%s' on node %u: is_param=%s",
                  m->param, ni->id, is_par ? "true" : "false");
    }
  }
}

/* --------------------------------------------------------------------------
 * Node lifecycle
 * -------------------------------------------------------------------------- */

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = on_node_param,
};

static void on_proxy_removed(void *data) {
  struct node_info *ni = data;
  log_verbose("Node %u ('%s') proxy removed", ni->id, ni->name);
  pw_proxy_destroy(ni->proxy);
}

static void on_proxy_destroy(void *data) {
  struct node_info *ni = data;
  struct data *d = ni->data;

  /* Capture id/name before we invalidate the slot */
  uint32_t dead_id = ni->id;
  char dead_name[MAX_NODE_NAME];
  snprintf(dead_name, sizeof(dead_name), "%s", ni->name);

  /* Remove spa_hooks while ni is still valid */
  spa_hook_remove(&ni->proxy_listener);
  spa_hook_remove(&ni->node_listener);

  /* Invalidate all mappings that referenced this node */
  for (int i = 0; i < d->n_mappings; i++) {
    if (d->mappings[i].node_id == dead_id) {
      d->mappings[i].node_id = SPA_ID_INVALID;
      d->mappings[i].prop_info_resolved = false;
      log_verbose("Mapping ch=%d cc=%d unlinked from node %u (destroyed)",
                  d->mappings[i].channel, d->mappings[i].control, dead_id);
    }
  }

  /* Swap-remove from node table — safe because ni is no longer used after */
  for (int i = 0; i < d->n_nodes; i++) {
    if (d->nodes[i].id == dead_id) {
      d->nodes[i] = d->nodes[--d->n_nodes];
      break;
    }
  }

  log_verbose("Node %u ('%s') destroyed and removed from table", dead_id,
              dead_name);
}

static const struct pw_proxy_events proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .removed = on_proxy_removed,
    .destroy = on_proxy_destroy,
};

/* Bind to a newly discovered node, wire up listeners, and resolve mappings */
static void bind_node(struct data *d, uint32_t id, const char *name) {
  if (d->n_nodes >= MAX_NODES) {
    log_warn("Node table full, ignoring node %u ('%s')", id, name);
    return;
  }

  struct node_info *ni = &d->nodes[d->n_nodes++];
  memset(ni, 0, sizeof(*ni));
  ni->id = id;
  ni->data = d;
  snprintf(ni->name, sizeof(ni->name), "%s", name);

  ni->proxy = pw_registry_bind(d->registry, id, PW_TYPE_INTERFACE_Node,
                               PW_VERSION_NODE, 0);
  if (!ni->proxy) {
    log_warn("Failed to bind proxy for node %u", id);
    d->n_nodes--;
    return;
  }

  pw_proxy_add_listener(ni->proxy, &ni->proxy_listener, &proxy_events, ni);
  pw_node_add_listener((struct pw_node *)ni->proxy, &ni->node_listener,
                       &node_events, ni);

  log_verbose("Bound node %u ('%s')", id, name);

  /* Resolve any mappings waiting for this node name */
  bool any = false;
  for (int i = 0; i < d->n_mappings; i++) {
    struct mapping *m = &d->mappings[i];
    if (strcmp(m->node_name, name) == 0) {
      m->node_id = id;
      m->prop_info_resolved = false;
      any = true;
      log_info("Mapping ch=%d cc=%d now linked to node %u ('%s')", m->channel,
               m->control, id, name);
    }
  }

  if (any) {
    /* Enumerate PropInfo so we can resolve is_param */
    pw_node_enum_params((struct pw_node *)ni->proxy, 0, SPA_PARAM_PropInfo, 0,
                        UINT32_MAX, NULL);
  }
}

/* --------------------------------------------------------------------------
 * Registry events
 * -------------------------------------------------------------------------- */

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
  struct data *d = data;

  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;

  const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (!name)
    return;

  log_verbose("Registry: node %u appeared: '%s'", id, name);

  /* Check if any mapping cares about this node */
  bool wanted = false;
  for (int i = 0; i < d->n_mappings; i++) {
    if (strcmp(d->mappings[i].node_name, name) == 0) {
      wanted = true;
      break;
    }
  }

  if (wanted)
    bind_node(d, id, name);
}

static void on_registry_global_remove(void *data, uint32_t id) {
  /* Handled via proxy_destroy callback */
  (void)data;
  (void)id;
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

/* --------------------------------------------------------------------------
 * Core sync — used to wait for the initial registry snapshot
 * -------------------------------------------------------------------------- */

static void on_core_done(void *data, uint32_t id, int seq) {
  struct data *d = data;
  if (id == PW_ID_CORE && seq == d->pending_seq) {
    /* Initial sync complete — warn about any still-unresolved mappings */
    for (int i = 0; i < d->n_mappings; i++) {
      struct mapping *m = &d->mappings[i];
      if (m->node_id == SPA_ID_INVALID)
        log_warn("Node '%s' not found in graph (will activate if it "
                 "appears later)",
                 m->node_name);
    }
  }
}

static void on_core_error(void *data, uint32_t id, int seq, int res,
                          const char *message) {
  struct data *d = data;
  log_error("Core error: id=%u seq=%d res=%d (%s): %s", id, seq, res,
            spa_strerror(res), message);
  if (id == PW_ID_CORE)
    pw_main_loop_quit(d->loop);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

/* --------------------------------------------------------------------------
 * MIDI handling via a filter node
 * -------------------------------------------------------------------------- */

struct midi_port_data {
  struct data *app;
  struct pw_filter *filter;
  struct spa_hook filter_listener;
  void *in_port; /* input MIDI port handle */
};

static void on_filter_process(void *userdata, struct spa_io_position *pos) {
  struct midi_port_data *mpd = userdata;
  struct data *d = mpd->app;
  (void)pos;

  struct pw_buffer *buf = pw_filter_dequeue_buffer(mpd->in_port);
  if (!buf)
    return;

  struct spa_buffer *sbuf = buf->buffer;
  uint32_t chunk_size = sbuf->datas[0].chunk->size;

  if (!sbuf->datas[0].data || chunk_size == 0)
    goto done;

  /* The actual data starts at chunk->offset within the data pointer */
  void *raw =
      SPA_PTROFF(sbuf->datas[0].data, sbuf->datas[0].chunk->offset, void);
  struct spa_pod *pod = raw;

  if (!spa_pod_is_sequence(pod))
    goto done;

  struct spa_pod_control *c;
  SPA_POD_SEQUENCE_FOREACH((struct spa_pod_sequence *)pod, c) {
    uint8_t status, msg_type, channel, controller, value;
    uint8_t midi[3];

    if (c->type == SPA_CONTROL_Midi) {
      uint8_t *raw = SPA_POD_BODY(&c->value);
      uint32_t size = SPA_POD_BODY_SIZE(&c->value);

      log_verbose("process: MIDI bytes=%u: %02x %02x %02x", size,
                  size > 0 ? raw[0] : 0, size > 1 ? raw[1] : 0,
                  size > 2 ? raw[2] : 0);

      if (size < 3)
        continue;
      midi[0] = raw[0];
      midi[1] = raw[1];
      midi[2] = raw[2];

    } else if (c->type == SPA_CONTROL_UMP) {
      uint32_t *ump = SPA_POD_BODY(&c->value);
      uint32_t size = SPA_POD_BODY_SIZE(&c->value);
      uint8_t ev[8];

      /*
      log_verbose("process: UMP bytes=%u: %02x %02x %02x %02x",
                  size,
                  size > 0 ? ((uint8_t*)ump)[0] : 0,
                  size > 1 ? ((uint8_t*)ump)[1] : 0,
                  size > 2 ? ((uint8_t*)ump)[2] : 0,
                  size > 3 ? ((uint8_t*)ump)[3] : 0);
      */

      int ev_size = spa_ump_to_midi(ump, size, ev, sizeof(ev));
      if (ev_size < 3)
        continue;

      midi[0] = ev[0];
      midi[1] = ev[1];
      midi[2] = ev[2];

    } else {
      continue;
    }

    status = midi[0];
    msg_type = status & 0xF0;
    channel = (status & 0x0F) + 1; /* make 1-based */

    /* We only care about Control Change (0xB0) */
    if (msg_type != 0xB0)
      continue;

    controller = midi[1];
    value = midi[2];

    log_verbose("MIDI CC: channel=%u controller=%u value=%u", channel,
                controller, value);

    /* Find matching mappings */
    for (int i = 0; i < d->n_mappings; i++) {
      struct mapping *m = &d->mappings[i];

      if (m->channel != (int)channel || m->control != (int)controller)
        continue;

      if (m->node_id == SPA_ID_INVALID) {
        log_verbose("No node for mapping ch=%d cc=%d yet, skipping", m->channel,
                    m->control);
        continue;
      }

      /* Find the node_info for this mapping */
      struct node_info *ni = NULL;
      for (int j = 0; j < d->n_nodes; j++) {
        if (d->nodes[j].id == m->node_id) {
          ni = &d->nodes[j];
          break;
        }
      }
      if (!ni)
        continue;

      double scaled = scale_midi(value, m->min, m->max);

      log_verbose("ch=%d cc=%d val=%d -> node='%s' "
                  "param='%s' scaled=%.4f (is_param=%s)\n",
                  m->channel, m->control, value, m->node_name, m->param, scaled,
                  m->prop_info_resolved ? (m->is_param ? "true" : "false")
                                        : "unresolved");

      schedule_node_param(d->pw_loop, ni->proxy, m->param, (float)scaled,
                          m->prop_info_resolved ? m->is_param : true);
    }
  }

done:
  pw_filter_queue_buffer(mpd->in_port, buf);
}

static void on_filter_state_changed(void *userdata, enum pw_filter_state old,
                                    enum pw_filter_state state,
                                    const char *error) {
  log_verbose("Filter state: %s -> %s%s%s", pw_filter_state_as_string(old),
              pw_filter_state_as_string(state), error ? ": " : "",
              error ? error : "");
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_filter_process,
    .state_changed = on_filter_state_changed,
};

/* --------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */
static struct pw_main_loop *g_loop = NULL;

static void handle_signal(int sig) {
  (void)sig;
  if (g_loop)
    pw_main_loop_quit(g_loop);
}

/* --------------------------------------------------------------------------
 * Usage / main
 * -------------------------------------------------------------------------- */
static void usage(const char *prog, FILE *out) {
  fprintf(
      out,
      "Usage: %s [OPTIONS] [config-file]\n"
      "\n"
      "Listen for MIDI CC events and map them to PipeWire node parameters.\n"
      "\n"
      "Options:\n"
      "  -v, --verbose     Print diagnostic information\n"
      "  -h, --help        Show this help\n"
      "\n"
      "config-file defaults to 'controls.json'\n",
      prog);
}

int main(int argc, char *argv[]) {
  static const struct option long_opts[] = {{"verbose", no_argument, NULL, 'v'},
                                            {"help", no_argument, NULL, 'h'},
                                            {NULL, 0, NULL, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "vh", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'v':
      verbose = true;
      break;
    case 'h':
      usage(argv[0], stdout);
      return 0;
    default:
      usage(argv[0], stderr);
      return 1;
    }
  }

  const char *config_path = (optind < argc) ? argv[optind] : "controls.json";

  /* ------------------------------------------------------------------ */
  pw_init(&argc, &argv);
  log_verbose("PipeWire version: %s", pw_get_library_version());

  struct data d = {0};
  struct midi_port_data mpd = {0};
  mpd.app = &d;

  /* Load config */
  int n = load_config(config_path, d.mappings, MAX_MAPPINGS);
  if (n < 0)
    return 1;
  d.n_mappings = n;

  /* Main loop + context + core */
  d.loop = pw_main_loop_new(NULL);
  if (!d.loop) {
    log_error("Failed to create main loop");
    return 1;
  }
  d.pw_loop = pw_main_loop_get_loop(d.loop);
  g_loop = d.loop;

  d.context = pw_context_new(pw_main_loop_get_loop(d.loop), NULL, 0);
  if (!d.context) {
    log_error("Failed to create context");
    return 1;
  }

  d.core = pw_context_connect(d.context, NULL, 0);
  if (!d.core) {
    log_error("Failed to connect to PipeWire daemon");
    return 1;
  }

  pw_core_add_listener(d.core, &d.core_listener, &core_events, &d);

  /* Registry */
  d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(d.registry, &d.registry_listener, &registry_events,
                           &d);

  /* Sync so we get the initial graph snapshot before proceeding */
  d.pending_seq = pw_core_sync(d.core, PW_ID_CORE, 0);

  /* MIDI filter node.
   *
   * Use pw_filter_new (not pw_filter_new_simple) so the filter shares our
   * existing core connection and participates in the same registry/loop.
   *
   * PW_KEY_MEDIA_CLASS "Midi/Sink" tells WirePlumber to assign a graph
   * driver so that PW_FILTER_FLAG_RT_PROCESS fires.
   *
   * We pass both EnumFormat (application/control) and Buffers pods at
   * port-add time so PipeWire can negotiate the link with Midi-Bridge.
   */
  struct pw_properties *filter_props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Midi", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_CLASS, "Midi/Sink", PW_KEY_NODE_NAME, "midi-control",
      PW_KEY_NODE_DESCRIPTION, "MIDI Control Mapper", NULL);

  mpd.filter = pw_filter_new(d.core, "midi-control", filter_props);
  if (!mpd.filter) {
    log_error("Failed to create filter");
    return 1;
  }

  pw_filter_add_listener(mpd.filter, &mpd.filter_listener, &filter_events,
                         &mpd);

  /* Build port params: EnumFormat + Buffers in one pod array */
  uint8_t pod_buf[1024];
  struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
  const struct spa_pod *port_params[2];

  port_params[0] = spa_pod_builder_add_object(
      &pb, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
      SPA_POD_Id(SPA_MEDIA_TYPE_application), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));

  port_params[1] = spa_pod_builder_add_object(
      &pb, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, 32),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
      SPA_POD_CHOICE_RANGE_Int(4096, 4096, INT32_MAX), SPA_PARAM_BUFFERS_stride,
      SPA_POD_Int(1));

  mpd.in_port = pw_filter_add_port(
      mpd.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
      pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME,
                        "midi_in", NULL),
      port_params, 2);

  if (!mpd.in_port) {
    log_error("Failed to add MIDI port");
    return 1;
  }

  if (pw_filter_connect(mpd.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
    log_error("Failed to connect filter");
    return 1;
  }

  /* Signal handlers for clean shutdown */
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  pw_main_loop_run(d.loop);

  /* Cleanup */
  log_verbose("Shutting down");
  pw_filter_destroy(mpd.filter);
  spa_hook_remove(&d.registry_listener);
  pw_proxy_destroy((struct pw_proxy *)d.registry);
  spa_hook_remove(&d.core_listener);
  pw_core_disconnect(d.core);
  pw_context_destroy(d.context);
  pw_main_loop_destroy(d.loop);
  pw_deinit();

  return 0;
}
