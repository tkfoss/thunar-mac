#!/bin/bash
# Submit a .zip/.dmg to Apple's notary service and wait for the result.
#
#   AC_API_KEY_ID=... AC_API_ISSUER_ID=... [AC_API_KEY_PATH=AuthKey.p8] macos/notarize.sh FILE
#
# Uses an App Store Connect API key (Users and Access > Integrations > Team Keys).
# AC_API_KEY_PATH defaults to $RUNNER_TEMP/AuthKey.p8 (GitHub Actions). Exits
# non-zero, after printing the notary log, unless the submission is Accepted.
# Staple afterwards with: xcrun stapler staple FILE (or the .app inside a zip).
set -euo pipefail

file="${1:?usage: $0 FILE}"
key="${AC_API_KEY_PATH:-${RUNNER_TEMP:-/tmp}/AuthKey.p8}"
: "${AC_API_KEY_ID:?AC_API_KEY_ID is not set}"
: "${AC_API_ISSUER_ID:?AC_API_ISSUER_ID is not set}"
[ -f "$key" ] || { echo "API key $key not found" >&2; exit 1; }

auth=(--key "$key" --key-id "$AC_API_KEY_ID" --issuer "$AC_API_ISSUER_ID")
result="$(xcrun notarytool submit "$file" "${auth[@]}" --wait --timeout 45m --output-format json)"
echo "$result"
id="$(echo "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))')"
status="$(echo "$result" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))')"
if [ "$status" != "Accepted" ]; then
  echo "notarization of $file: $status" >&2
  [ -n "$id" ] && xcrun notarytool log "$id" "${auth[@]}" >&2 || true
  exit 1
fi
echo "notarized $file ($id)"
