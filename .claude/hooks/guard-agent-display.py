#!/usr/bin/env python3
"""PreToolUse guard: keep agent app launches off the user's real display.

Sumi is a GUI app. Launching it directly puts a window on the user's session
and steals focus. Everything must go through tools/agent-display.sh, which runs
it in a nested/headless cage compositor. CLAUDE.md says so, but an instruction
is only an instruction - this makes it actually hold.

Denies rather than rewrites: the reason string is fed back to the model, which
then retries through the wrapper. Rewriting the command silently would hide the
rule and make a genuine manual launch impossible to express.
"""
import json
import re
import sys

# ./Release/Sumi, syncscribble/Debug/Sumi (or a leftover Kaku/Write build), absolute paths, with or without args.
LAUNCH = re.compile(r"(?:^|[\s;&|(`])[^\s;&|]*(?:Release|Debug)/(?:Sumi|Kaku|Write)\b")
# Anything routed through the wrapper is fine, as is merely talking about the
# path (grep, ls, test -x, rm) rather than executing it.
ALLOWED = re.compile(
    r"agent-display\.sh"
    # Inspecting, listing or signalling the binary is not launching it. pgrep and
    # pkill matter in particular: process cleanup names the path but never runs it.
    r"|(?:^|[\s;&|])(?:ls|grep|rg|find|stat|file|cat|head|tail|rm|cp|mv|chmod|strip"
    r"|nm|objdump|size|ldd|test|\[|pgrep|pkill|killall|kill|ps|echo|printf)\b"
)


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0  # Never block on a malformed payload.

    if payload.get("tool_name") != "Bash":
        return 0

    command = payload.get("tool_input", {}).get("command", "")
    if not LAUNCH.search(command) or ALLOWED.search(command):
        return 0

    reason = (
        "Launching Sumi directly would open a window on the user's own niri "
        "session and steal focus. Use the isolated compositor instead:\n"
        "  tools/agent-display.sh run          # headless cage session\n"
        "  tools/agent-display.sh shot out.png # look at it\n"
        "  tools/agent-display.sh click X Y | type TEXT | key ctrl+s\n"
        "  tools/agent-display.sh test         # ./Debug/Sumi --test\n"
        "  tools/agent-display.sh stop\n"
        "If the user explicitly asked for a visible window, add --windowed."
    )
    json.dump(
        {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": reason,
            }
        },
        sys.stdout,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
