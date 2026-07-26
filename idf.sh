#!/usr/bin/env bash

set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
    default_idf_path="${HOME}/esp/esp-idf"
    if [[ -d "${default_idf_path}" ]]; then
        export IDF_PATH="${default_idf_path}"
    else
        echo "IDF_PATH is not set and default path was not found: ${default_idf_path}" >&2
        exit 1
    fi
fi

# ESP-IDF 5.5.x may reject toolchains with the same GCC version but a different
# package date stamp during configure regeneration. Export the override before
# invoking idf.py so the check stays non-fatal across the whole build.
export IDF_MAINTAINER="${IDF_MAINTAINER:-1}"

# shellcheck disable=SC1090
. "${IDF_PATH}/export.sh" >/dev/null

exec idf.py "$@"
