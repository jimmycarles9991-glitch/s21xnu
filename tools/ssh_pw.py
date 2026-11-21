#!/usr/bin/env python3
"""Run ssh/scp with a password fed from S21_SSH_PASSWORD (default: root).

Usage: ssh_pw.py <ssh|scp> [arguments...]

Small pexpect wrapper so boot scripts do not need sshpass/expect. It answers the
first password prompt (and the initial host-key question if it appears), then
streams the child's output until EOF and returns the child's exit status.
"""
import os
import sys

import pexpect


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: ssh_pw.py <ssh|scp> [arguments...]", file=sys.stderr)
        return 2

    prog, args = sys.argv[1], sys.argv[2:]
    password = os.environ.get("S21_SSH_PASSWORD", "root")

    child = pexpect.spawn(prog, args, timeout=None, encoding=None, codec_errors="replace")
    child.logfile_read = sys.stdout.buffer

    prompts = [
        b"Are you sure you want to continue connecting",
        b"(?i)password:",
        b"(?i)passphrase:",
        pexpect.EOF,
    ]
    while True:
        i = child.expect(prompts, timeout=60)
        if i == 0:
            child.sendline(b"yes")
        elif i in (1, 2):
            child.sendline(password.encode())
            break
        else:
            child.close()
            if child.exitstatus is not None:
                return child.exitstatus
            if child.signalstatus is not None:
                return 128 + child.signalstatus
            return 1

    child.expect(pexpect.EOF)
    child.close()
    if child.exitstatus is not None:
        return child.exitstatus
    if child.signalstatus is not None:
        return 128 + child.signalstatus
    return 1


if __name__ == "__main__":
    sys.exit(main())
