#!/usr/bin/env bash
set -euo pipefail

ROOT="${HOME}/virtio-balloon"
QMP="${ROOT}/logs/qmp.sock"
LOGI="/tmp/phase2_qmp_inflate.log"
LOGD="/tmp/phase2_qmp_deflate.log"

if [[ ! -S "${QMP}" ]]; then
  echo "ERROR: QMP socket not found at ${QMP}"
  echo "Start QEMU first: ./scripts/run_qemu_phase2.sh"
  exit 1
fi

echo "[1/4] Build host daemon"
make -C "${ROOT}/host/balloond" clean >/dev/null
make -C "${ROOT}/host/balloond" >/dev/null

echo "[2/4] Inflate test -> 2 GiB"
timeout 60s "${ROOT}/host/balloond/balloond" 2147483648 "${QMP}" < /dev/null > "${LOGI}" 2>&1 || true
echo "----- BEGIN INFLATE LOG -----"
cat "${LOGI}" || true
echo "----- END INFLATE LOG -----"
grep -q "actual=2147483648 target=2147483648" "${LOGI}"

echo "[3/4] Deflate test -> 3 GiB"
timeout 30s "${ROOT}/host/balloond/balloond" 3221225472 "${QMP}" < /dev/null > "${LOGD}" 2>&1 || true
echo "----- BEGIN DEFLATE LOG -----"
cat "${LOGD}" || true
echo "----- END DEFLATE LOG -----"
grep -q "actual=3221225472 target=3221225472" "${LOGD}"

echo "[4/4] Save proofs"
mkdir -p "${ROOT}/proofs"
cp "${LOGI}" "${ROOT}/proofs/inflate_convergence.log"
cp "${LOGD}" "${ROOT}/proofs/deflate_convergence.log"

echo "smoke test: completed (real QMP path)"
