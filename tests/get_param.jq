# Searches Props array for a parameter by name
# Usage: jq --arg param_name "volume" -f get_param.jq
#
# Searches both:
# - Direct properties (e.g., "volume", "mute")
# - Parameters in params array (e.g., "gain:Gain 1")

def find_in_params($key):
  .params as $arr |
  if $arr then
    # Iterate through params array by pairs (index 0, 2, 4, ...)
    [range(0; $arr | length; 2)] |
    map(. as $i | if $arr[$i] == $key then $arr[$i + 1] else empty end) |
    first
  else
    null
  end;

# Search all Props elements
[
  .Props[] |
  if has($param_name) then
    .[$param_name]
  else
    find_in_params($param_name)
  end
] |
map(select(. != null)) |
if length > 0 then .[0] else null end
