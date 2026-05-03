#!/usr/bin/env python3

import argparse
import json
import shlex
import subprocess
import sys


def parse_flags(flags, result, *, for_link):
  i = 0
  while i < len(flags):
    flag = flags[i]
    if flag == "-isystem" and i + 1 < len(flags):
      result["cflags_cc"].extend([flag, flags[i + 1]])
      i += 2
      continue
    if flag.startswith("-I"):
      result["include_dirs"].append(flag[2:])
    elif flag.startswith("-L"):
      result["lib_dirs"].append(flag[2:])
    elif flag.startswith("-l"):
      result["libs"].append(flag[2:])
    elif flag == "-pthread":
      if for_link:
        result["ldflags"].append(flag)
      else:
        result["cflags"].append(flag)
    elif flag.startswith("-Wl,-rpath,"):
      # Homebrew and the GStreamer framework emit absolute rpaths. They make the
      # plugin non-relocatable, so keep only loader-relative rpaths from pkg-config.
      rpath = flag[len("-Wl,-rpath,"):]
      if rpath.startswith("@"):
        result["ldflags" if for_link else "cflags"].append(flag)
    elif flag.startswith("-Wl,") or flag.startswith("-F"):
      result["ldflags" if for_link else "cflags"].append(flag)
    else:
      result["ldflags" if for_link else "cflags"].append(flag)
    i += 1


def pkg_config(pkg_config, mode, packages):
  return shlex.split(
      subprocess.check_output(
          [pkg_config, mode] + packages,
          stderr=subprocess.STDOUT,
          text=True,
      ))


def main() -> int:
  parser = argparse.ArgumentParser()
  parser.add_argument("--pkg-config", default="pkg-config")
  parser.add_argument("packages", nargs="+")
  args = parser.parse_args()

  result = {
      "include_dirs": [],
      "cflags": [],
      "cflags_cc": [],
      "lib_dirs": [],
      "libs": [],
      "ldflags": [],
  }

  try:
    parse_flags(pkg_config(args.pkg_config, "--cflags", args.packages), result,
                for_link=False)
    parse_flags(pkg_config(args.pkg_config, "--libs", args.packages), result,
                for_link=True)
  except (subprocess.CalledProcessError, FileNotFoundError) as exc:
    output = getattr(exc, "output", "") or str(exc)
    sys.stderr.write("failed to run pkg-config for GStreamer: " + output + "\n")
    return 1

  print(json.dumps(result))
  return 0


if __name__ == "__main__":
  sys.exit(main())
