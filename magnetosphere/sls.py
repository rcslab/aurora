from configparser import ConfigParser
from pathlib import Path
import subprocess as sp
from typing import Dict, List


def bashcmd(lst, fail_okay=False, mute=False):
    if fail_okay:
        # Propagate the return code upwards
        ret = sp.run(lst) if not mute else sp.run(lst, stdout=sp.DEVNULL)
        return ret.returncode
    else:
        # Throw an exception if there was an error
        if mute:
            sp.run(lst, stdout=sp.DEVNULL).check_returncode()
        else:
            sp.run(lst).check_returncode()
        return 0

# Insert a series of PIDs into a partition and start checkpointing them.


def checkpoint(pid: int, options: Dict[str, str]):

    slsctl = str(Path(options["sls_source"], "tools", "slsctl", "slsctl"))
    oid = options["oid"]

    partadd = [slsctl, "partadd", "-o", oid, "-b",
               options["backend"], "-t", options["period"]]
    if options["delta"]:
        partadd.append("-d")
    bashcmd(partadd)

    attach = [slsctl, "attach", "-o", oid, "-p", str(pid)]
    bashcmd(attach)

    checkpoint = [slsctl, "checkpoint", "-r", "-o", oid]
    bashcmd(checkpoint)

    print("Started Aurora Checkpointer on {}".format(pid))


def load_modules(source_root: Path):
    sls = source_root / "sls" / "sls.ko"
    slos = source_root / "slos" / "slos.ko"
    bashcmd(["kldload", str(slos)])
    bashcmd(["kldload", str(sls)])


def create_root(parser: ConfigParser):
    """Create the file system and populate it with the root."""
    slsoptions = parser["sls"]

    sls_source = slsoptions["sls_source"]
    mount_point = Path(slsoptions["mount_point"])
    disk_path = slsoptions["disk_path"]
    tarfile = slsoptions["tarfile"]
    newfs = str(Path(sls_source, "tools", "newfs_sls", "newfs_sls"))

    bashcmd([newfs, disk_path])
    bashcmd(["mount", "-t", "slsfs", disk_path, str(mount_point)])
    bashcmd(["tar", "-xf", tarfile, "-C", mount_point])
    bashcmd(["mount", "-t", "devfs", "devfs", str(mount_point / "dev")])


def teardown(root: Path):
    bashcmd(["kldunload", "sls"], fail_okay=True)
    bashcmd(["umount", str(root / "dev")], fail_okay=True)
    bashcmd(["umount", str(root)], fail_okay=True)
    bashcmd(["kldunload", "slos"], fail_okay=True)


def gstripe(name: str, stripesize: int, disks: List[int]) -> None:
    bashcmd(["gstripe", "load"], fail_okay=True)
    bashcmd(["gstripe", "destroy", name], fail_okay=True)
    bashcmd(["gstripe", "create", "-s", str(stripesize), name] + disks)
