"""Send commands to the runtime's virtual pad (\\\\.\\pipe\\FlamingoPad), as if pressed on a controller.

The runtime adds them to the real controllers' input (shims/pad.cpp). Commands, one per argument or line:
    press <button> [ms]      a b x y up down left right lb rb ls rs menu view   (default 120 ms)
    hold <button> / release <button>
    stick l|r <x> <y> [ms]   -1..1, +y up
    trigger l|r <0..1> [ms]
    wait <ms>                (handled here: a pause between commands)

Usage:
    python -I pad.py "press a"
    python -I pad.py "press down" "wait 300" "press down" "wait 300" "press a"
"""
import sys
import time

PIPE = r"\\.\pipe\FlamingoPad"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    try:
        pipe = open(PIPE, "w", encoding="ascii")
    except OSError as e:
        print(f"the game isn't running with the virtual pad ({e})")
        return 1
    with pipe:
        for cmd in sys.argv[1:]:
            words = cmd.split()
            if words and words[0] == "wait":
                pipe.flush()
                time.sleep(float(words[1]) / 1000 if len(words) > 1 else 0.3)
                continue
            pipe.write(cmd.strip() + "\n")
            pipe.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
