#!/bin/bash

check_param_val() {
  local expected=$1
  local have

  have=$(pw-dump | jq -r '.[] | select(.info.props."node.name" == "gain.input").info.params.Props[1].params[1]')

  echo "have $have, expected $expected"
  [[ "$have" = "$expected" ]]
}

check_prop_val() {
  local expected=$1
  local have

  have=$(pw-dump | jq -r '.[] | select(.info.props."node.name" == "gain.input").info.params.Props[0].volume')

  echo "have $have, expected $expected"
  [[ "$have" = "$expected" ]]
}

if ! [[ -x midictl ]]; then
  echo "You need to build midictl first." >&2
  exit 1
fi

./midictl -v tests/controls.json >&tests/midictl.log &
midictl_pid=$!
wpexec tests/gain.lua >&tests/wpexec.log &
wpexec_pid=$!
trap "kill $midictl_pid $wpexec_pid" exit

# wait for midictl port to show up
if ! timeout 10 sh -c 'while ! pw-link -io | grep -q midi-control:midi_in; do sleep 1; done'; then
  echo "ERROR: timed out waiting for midictl port" >&2
  exit 1
fi

if ! timeout 10 sh -c 'while ! pw-link -io | grep -q gain.input; do sleep 1; done'; then
  echo "ERROR: timed out waiting for gain.input port" >&2
  exit 1
fi

# link midi input to midictl
pw-link 'Midi-Bridge:Midi Through Port-0 (capture)' 'midi-control:midi_in'

# get node id for gain.input
nodeid=$(pw-dump | jq -r '.[] | select(.info.props."node.name" == "gain.input").id')
echo "found gain.input with id $nodeid"

# reset test parameters
pw-cli set-param "$nodeid" 2 '{"volume": 1.0}'
pw-cli set-param "$nodeid" 2 '{"params": ["gain:Gain 1", 1.0]}'

if ! check_param_val 1.000000; then
  echo "ERROR: failed to reset parameter value" >&2
  exit 1
fi

if ! check_prop_val 1.000000; then
  echo "ERROR: failed to reset direct property value" >&2
  exit 1
fi

sendmidi dev 'Midi Through Port-0' cc 23 127
sleep 2
if ! check_param_val 10.000000; then
  echo "ERROR: failed to set parameter value" >&2
  exit 1
fi

sendmidi dev 'Midi Through Port-0' cc 24 127
sleep 2
if ! check_prop_val 10.000000; then
  echo "ERROR: failed to set direct property value" >&2
  exit 1
fi

echo "TEST SUCCESSFUL"
