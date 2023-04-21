# @file zcbor_gen.py
#
# @brief Generate C files from cddl
#

import os
from pathlib import Path
import subprocess

# Create encode and decode at the same time
# This is coupled to CONFIG_SHELL_ARGC_MAX (12)
options = "--default-max-qty 12 --short-names -d -e -t"


def generate(lst: list):
    script_dir = Path(os.path.dirname(os.path.abspath(__file__)))
    api_path = script_dir.joinpath("shell_mgmt.cddl")
    for x in lst:
        src_path = script_dir.joinpath("./source/" + x + ".c")
        inc_path = script_dir.joinpath("./include/" + x + ".h")
        cmd = f"zcbor code -c {api_path} {options} {x} --oc {src_path} --oh {inc_path}"
        print(cmd)
        subprocess.call(cmd, shell=True)


if __name__ == "__main__":
    # At this time everything is command/response
    api = ["shell_exec_cmd", "shell_exec_rsp"]
    generate(api)
