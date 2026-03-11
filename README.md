# midictl: map midi events to pipewire parameter changes

This maps MIDI control change events to pipewire parameter changes. For example, if you want midi control 23 on channel 1 to control the `gain:Gain 1` parameter of pipewire node `gain.input`, you would create a configuration file like this:

```json
[
  { "channel": 1, "control": 23, "node": "gain.input", "param": "gain:Gain 1", "min": 0, "max": 10 }
]
```

This specifies the channel and control value, pipewire node and parameter name, and a target range. The midi value will be scaled to the target range.
