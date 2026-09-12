#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# vm.sh - thin wrapper for the GD32H759 openvela contest build VM.
#
# Why this exists: the raw ssh/scp invocations need four non-obvious options
# (key path, BatchMode, StrictHostKeyChecking, ConnectTimeout) and two caller
# habits (one file per scp, never a multi-line command string).  Getting any of
# them wrong fails confusingly, so they are encoded here once.
#
# Usage:
#   vm.sh shell                      interactive login
#   vm.sh run  '<command>'           run one single-line command on the VM
#   vm.sh put  <local> <remote>      upload one file
#   vm.sh get  <remote> <local>      download one file
#   vm.sh script <local.sh> [args]   upload a script and run it (use this for
#                                    anything multi-line)
#   vm.sh build                      run the firmware build on the VM
#
# Override the target with VM_HOST / VM_KEY if the VM moves.

set -uo pipefail

VM_HOST="${VM_HOST:-topeet@192.168.205.128}"
VM_KEY="${VM_KEY:-$HOME/.ssh/openvela_vm}"
SSH_OPTS=(-i "$VM_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=15)

usage() { sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

need_key() {
  if [ ! -f "$VM_KEY" ]; then
    echo "vm.sh: private key not found: $VM_KEY" >&2
    echo "vm.sh: the VM also accepts a password, but this wrapper cannot use it -" >&2
    echo "vm.sh: ssh only prompts on a TTY, and tool-driven shells have none." >&2
    echo "vm.sh: install the public key on the VM first, or run ssh by hand." >&2
    exit 3
  fi
}

cmd="${1:-}"; shift || usage
need_key

case "$cmd" in
  shell)
    exec ssh "${SSH_OPTS[@]}" "$VM_HOST"
    ;;

  run)
    [ $# -ge 1 ] || usage
    # A single argument: passing several lets the caller accidentally smuggle a
    # newline in, which the layered shells mangle.  Use 'vm.sh script' instead.
    exec ssh "${SSH_OPTS[@]}" "$VM_HOST" "$1"
    ;;

  put)
    [ $# -eq 2 ] || usage
    # One file per invocation: a space-separated list in one quoted string is
    # treated by scp as a single filename.
    scp -i "$VM_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no \
        "$1" "$VM_HOST:$2"
    ;;

  get)
    [ $# -eq 2 ] || usage
    scp -i "$VM_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no \
        "$VM_HOST:$1" "$2"
    ;;

  script)
    [ $# -ge 1 ] || usage
    local_file="$1"; shift
    [ -f "$local_file" ] || { echo "vm.sh: no such script: $local_file" >&2; exit 3; }
    base="$(basename "$local_file")"
    scp -i "$VM_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no \
        "$local_file" "$VM_HOST:/tmp/$base" || exit $?
    exec ssh "${SSH_OPTS[@]}" "$VM_HOST" "bash /tmp/$base $*"
    ;;

  build)
    exec ssh "${SSH_OPTS[@]}" "$VM_HOST" 'bash /tmp/build_fw.sh'
    ;;

  *)
    usage
    ;;
esac
