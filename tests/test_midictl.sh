#!/bin/bash

check_prop_val() {
  local param_name=$1
  local expected=$2
  local have

  have=$(pw-dump | jq -r '.[] | select(.info.props."node.name" == "gain.input").info.params' |
    jq -r --arg param_name "$param_name" -f tests/get_param.jq)

  echo "have $have, expected $expected"
  [[ "$have" = "$expected" ]]
}

if ! [[ -x midictl ]]; then
  echo "ERROR: you need to build midictl first." >&2
  exit 1
fi

./midictl -v tests/controls.json >&tests/midictl.log &
midictl_pid=$!
wpexec tests/gain.lua >&tests/wpexec.log &
wpexec_pid=$!
trap "kill $midictl_pid $wpexec_pid; wait" EXIT

# wait for midictl port to show up
echo -n "waiting for midictl: "
if ! timeout 10 sh -c 'while ! pw-link -io | grep -q midi-control:midi_in; do sleep 1; done'; then
  echo failed
  echo "ERROR: timed out waiting for midictl port" >&2
  exit 1
fi
echo ok

echo -n "waiting for gain plugin: "
if ! timeout 10 sh -c 'while ! pw-link -io | grep -q gain.input; do sleep 1; done'; then
  echo failed
  echo "ERROR: timed out waiting for gain.input port" >&2
  exit 1
fi
echo ok

#PS1="DEBUG> " bash --norc

# link midi input to midictl
pw-link 'Midi-Bridge:Midi Through Port-0 (capture)' 'midi-control:midi_in'

# get node id for gain.input
nodeid=$(pw-dump | jq -r '.[] | select(.info.props."node.name" == "gain.input").id')
echo "found gain.input with id $nodeid"

# reset test parameters
echo "resetting parameters to known values"
pw-cli set-param "$nodeid" 2 '{"volume": 1.0}' >/dev/null
pw-cli set-param "$nodeid" 2 '{"mute": false}' >/dev/null
pw-cli set-param "$nodeid" 2 '{"params": ["gain:Gain 1", 1.0]}' >/dev/null

echo -n "checking initial value of gain:Gain 1: "
if ! check_prop_val 'gain:Gain 1' 1.000000; then
  echo "ERROR: failed to reset parameter value" >&2
  exit 1
fi

echo -n "checking initial value of volume: "
if ! check_prop_val volume 1.000000; then
  echo "ERROR: failed to reset direct property value" >&2
  exit 1
fi

echo "sending midi cc to update gain"
sendmidi dev 'Midi Through Port-0' cc 23 127
sleep 2
echo -n "checking value of gain:Gain 1: "
if ! check_prop_val 'gain:Gain 1' 10.000000; then
  echo "ERROR: failed to set parameter value" >&2
  exit 1
fi

echo "Sending midi cc to update volume"
sendmidi dev 'Midi Through Port-0' cc 24 127
sleep 2
echo -n "checking value of volume: "
if ! check_prop_val volume 10.000000; then
  echo "ERROR: failed to set direct property value" >&2
  exit 1
fi

echo "sending midi cc 25 (button/mute) with value 127 -> expect mute=true (1)"
sendmidi dev 'Midi Through Port-0' cc 25 127
sleep 2
echo -n "checking value of mute (button, cc=127): "
if ! check_prop_val mute true; then
  echo "ERROR: button mapping (cc=127) did not set mute=true" >&2
  exit 1
fi

echo "sending midi cc 25 (button/mute) with value 0 -> expect mute=false (0)"
sendmidi dev 'Midi Through Port-0' cc 25 0
sleep 2
echo -n "checking value of mute (button, cc=0): "
if ! check_prop_val mute false; then
  echo "ERROR: button mapping (cc=0) did not set mute=false" >&2
  exit 1
fi

echo "sending midi cc 26 (button+invert/mute) with value 127 -> expect mute=false (0)"
sendmidi dev 'Midi Through Port-0' cc 26 127
sleep 2
echo -n "checking value of mute (button+invert, cc=127): "
if ! check_prop_val mute false; then
  echo "ERROR: button+invert mapping (cc=127) did not set mute=false" >&2
  exit 1
fi

echo "sending midi cc 26 (button+invert/mute) with value 0 -> expect mute=true (1)"
sendmidi dev 'Midi Through Port-0' cc 26 0
sleep 2
echo -n "checking value of mute (button+invert, cc=0): "
if ! check_prop_val mute true; then
  echo "ERROR: button+invert mapping (cc=0) did not set mute=true" >&2
  exit 1
fi

echo "TEST SUCCESSFUL"
