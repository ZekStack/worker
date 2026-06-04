#!/usr/bin/env python3

import argparse
import subprocess
from pathlib import Path


def git_lines(args):
    completed = subprocess.run(["git", *args], check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        return []
    return [line.strip() for line in completed.stdout.splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-ref", required=True)
    parser.add_argument("--tag-name", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    tags = git_lines(["tag", "--sort=-creatordate"])
    previous = next((tag for tag in tags if tag != args.tag_name), "")
    if previous:
        commits = git_lines(["log", "--pretty=format:%s", f"{previous}..{args.target_ref}"])
    else:
        commits = git_lines(["log", "--pretty=format:%s", args.target_ref])

    lines = [f"# {args.tag_name}", ""]
    if previous:
        lines.append(f"Changes since `{previous}`.")
        lines.append("")
    if commits:
        lines.extend(f"- {commit}" for commit in commits)
    else:
        lines.append("- Release maintenance.")
    lines.append("")

    Path(args.output).write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
