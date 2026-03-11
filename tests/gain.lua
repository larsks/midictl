local args_string = [[
{
  "media.name": "Gain (Mono)",
  "filter.graph": {
    "nodes": [
      {
        "type": "builtin",
        "name": "gain",
        "label": "mixer",
        "control": {
          "Gain 1": 4
        }
      }
    ],
    "inputs": [ "gain:In 1" ],
    "outputs": [ "gain:Out" ]
  },
  "capture.props": {
    "node.name": "gain.input",
    "media.class": "Audio/Sink"
  },
  "playback.props": {
    "node.name": "gain.output",
    "media.class": "Audio/Source"
  },
  "node.passive": true,
  "audio.position": [ "MONO" ],
  "node.description": "Gain (Mono)",
  "audio.channels": 1
}
]]
local properties = {}
filter_chain = LocalModule("libpipewire-module-filter-chain", args_string, properties)
