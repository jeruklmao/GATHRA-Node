#!/bin/sh
set -eu

if command -v gcc >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
  exec pio test -e native "$@"
fi

platformio_python="${PLATFORMIO_CORE_DIR:-${HOME}/.platformio}/penv/bin/python"
if ! "${platformio_python}" -c 'import ziglang' >/dev/null 2>&1; then
  echo "No host GCC found. Install fallback: ${platformio_python} -m pip install ziglang==0.15.2" >&2
  exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PATH="${script_dir}/native-bin:${PATH}"
export PATH
exec pio test -e native "$@"
