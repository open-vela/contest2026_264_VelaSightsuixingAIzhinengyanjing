#!/bin/sh

set -eu

script_dir=$(dirname "$(readlink -f "$0")")
csv=${1:-$script_dir/../../../../external/bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv}
generated=${2:-${BK7258_ARMINO_RAM_HEADER:-}}
header=$script_dir/hardware/bk7258_mbox.h
linker=$script_dir/../../boards/bk7258/bk7258-ap/scripts/ld.script

if [ ! -r "$csv" ]; then
  printf '%s\n' "BK7258 SWAP layout: cannot read $csv" >&2
  exit 1
fi

result=$(awk -F, '
  function hex(value, result, i, digit) {
    sub(/^0[xX]/, "", value)
    value = tolower(value)
    result = 0
    for (i = 1; i <= length(value); i++) {
      digit = index("0123456789abcdef", substr(value, i, 1)) - 1
      if (digit < 0) return -1
      result = result * 16 + digit
    }
    return result
  }
  BEGIN { base = hex("28000000"); found = 0 }
  /^[[:space:]]*#/ { next }
  /^[[:space:]]*PSRAM_CAPCAITY_SIZE/ { next }
  NF >= 4 {
    name = $1
    type = $2
    offset = $3
    size = $4
    gsub(/[[:space:]]/, "", name)
    gsub(/[[:space:]]/, "", type)
    gsub(/[[:space:]]/, "", offset)
    gsub(/[[:space:]]/, "", size)
    if (type != "SRAM") next
    if (offset != "") base = hex(offset)
    bytes = hex(size)
    if (name == "SWAP") {
      printf "0x%08x 0x%08x\n", base, bytes
      found = 1
    }
    base += bytes
  }
  END { if (!found) exit 2 }
' "$csv")

header_result=$(awk '
  /#define[[:space:]]+BK7258_SWAP_BASE/ { gsub(/[uUlL]/, "", $3); base=tolower($3) }
  /#define[[:space:]]+BK7258_SWAP_SIZE/ { gsub(/[uUlL]/, "", $3); size=tolower($3) }
  END { print base, size }
' "$header")

if [ "$header_result" != "$result" ]; then
  printf '%s\n' "BK7258 header SWAP mismatch: CSV=$result header=$header_result" >&2
  exit 1
fi

uart_result=$(awk '
  /#define[[:space:]]+BK7258_MB_UART_RX_ADDRESS/ { gsub(/[uUlL]/, "", $3); rx=tolower($3) }
  /#define[[:space:]]+BK7258_MB_UART_TX_ADDRESS/ { gsub(/[uUlL]/, "", $3); tx=tolower($3) }
  END { print rx, tx }
' "$header")
expected_uart=$(awk -v layout="$result" '
  function hex(value, result, i, digit) {
    sub(/^0[xX]/, "", value)
    value = tolower(value)
    result = 0
    for (i = 1; i <= length(value); i++) {
      digit = index("0123456789abcdef", substr(value, i, 1)) - 1
      if (digit < 0) return -1
      result = result * 16 + digit
    }
    return result
  }
  BEGIN {
    split(layout, fields, " ")
    base = hex(fields[1])
    printf "0x%08x 0x%08x\n", base + 1024, base + 1280
  }
')
if [ "$uart_result" != "$expected_uart" ]; then
  printf '%s\n' "BK7258 UART0 SWAP mismatch: expected=$expected_uart header=$uart_result" >&2
  exit 1
fi

linker_result=$(awk '
  /SWAP[[:space:]]+\(rw\)/ {
    for (i = 1; i <= NF; i++) {
      if ($i == "ORIGIN") base=$(i + 2)
      if ($i == "LENGTH") size=$(i + 2)
    }
    gsub(/,/, "", base)
    print tolower(base), tolower(size)
  }
' "$linker")

if [ "$linker_result" != "$result" ]; then
  printf '%s\n' "BK7258 linker SWAP mismatch: CSV=$result linker=$linker_result" >&2
  exit 1
fi

if [ -n "$generated" ]; then
  if [ ! -r "$generated" ]; then
    printf '%s\n' "BK7258 generated RAM header unreadable: $generated" >&2
    exit 1
  fi

  generated_result=$(awk '
    /#define[[:space:]]+CONFIG_SWAP_ADDR/ { gsub(/[uUlL]/, "", $3); base=tolower($3) }
    /#define[[:space:]]+CONFIG_SWAP_SIZE/ { gsub(/[uUlL]/, "", $3); size=tolower($3) }
    END { print base, size }
  ' "$generated")
  if [ "$generated_result" != "$result" ]; then
    printf '%s\n' "BK7258 generated RAM header mismatch: CSV=$result generated=$generated_result" >&2
    exit 1
  fi
fi
