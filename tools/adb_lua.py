"""Stage a private startup script, run a separate device probe, or read the game's Lua log."""
import argparse
import base64
import hashlib
from pathlib import Path
import shlex
import subprocess
import uuid


PACKAGE = "io.github.iamwilmar.mhriseport"
ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--adb", default="adb")
    p.add_argument("-s", "--serial")
    commands = p.add_subparsers(dest="action", required=True)
    for name in ("stage", "stage-live", "probe"):
        cmd = commands.add_parser(name)
        cmd.add_argument("script", nargs="?", type=Path, default=ROOT/"tools/tdb_probe.lua")
        if name == "probe":
            cmd.add_argument("--exefs", default="/sdcard/MHRise/exefs")
        else:
            cmd.add_argument("--as", dest="relative", help="Path under autorun, e.g. modules/common.lua")
    commands.add_parser("log")
    commands.add_parser("reload", help="Reload autorun and live scripts on the game thread")
    commands.add_parser("save-config", help="Dispatch re.on_config_save on the game thread")
    mod = commands.add_parser("stage-mod", help="Install a PC entry script and its adjacent Lua module directory")
    mod.add_argument("entry", type=Path)
    commands.add_parser("stage-player", help="Install the build-specific live probe for the next game start")
    commands.add_parser("player", help="Request one live lookup on the next PlayerManager.update")
    yun = commands.add_parser("stage-yun", help="Stage PC yun_modules and the read-only data sampler")
    yun.add_argument("yunwulian", type=Path, help="Directory containing yun_modules.lua")
    commands.add_parser("yun", help="Sample yun_modules on the next PlayerManager.update")
    args = p.parse_args()
    adb = [args.adb] + (["-s", args.serial] if args.serial else [])
    def run(*values, **kwargs):
        return subprocess.run([*adb, *map(str, values)], check=True, **kwargs)

    def stage(source, dest):
        data = source.read_bytes()
        tmp = dest + "." + uuid.uuid4().hex
        script = ("mkdir -p " + shlex.quote(dest.rsplit("/", 1)[0]) +
                  " && base64 -d > " + shlex.quote(tmp) +
                  " && sha256sum " + shlex.quote(tmp))
        private = "run-as " + PACKAGE + " sh -c "
        try:
            result = run("shell", "-T", private + shlex.quote(script),
                         input=base64.b64encode(data), capture_output=True)
            if result.stdout.split()[0].decode() != hashlib.sha256(data).hexdigest():
                raise RuntimeError("Staged Lua file checksum mismatch")
            run("shell", private + shlex.quote("mv " + shlex.quote(tmp) + " " + shlex.quote(dest)))
        finally:
            run("shell", private + shlex.quote("rm -f " + shlex.quote(tmp)))

    if args.action in ("stage-yun","yun"):
        target="files/reframework"
        if args.action == "stage-yun":
            stage(args.yunwulian/"yun_modules.lua",target+"/autorun/yunwulian/yun_modules.lua")
            for file in sorted((args.yunwulian/"yun_modules").glob("*.lua")):
                name="ui.lua" if file.name=="UI.lua" else file.name
                stage(file,target+"/autorun/yunwulian/yun_modules/"+name)
            stage(ROOT/"tools/yun_data_probe.lua",target+"/yun_data_probe.lua")
        else:
            run("shell","run-as",PACKAGE,"touch",target+"/yun.request")
    elif args.action in ("stage-player", "player"):
        target = "files/reframework"
        if args.action == "stage-player":
            stage(ROOT/"tools/player_probe.lua",target+"/player_probe.lua")
        else:
            run("shell", "run-as", PACKAGE, "touch", target+"/player.request")
    elif args.action in ("reload","save-config"):
        marker = "reload" if args.action=="reload" else "config"
        run("shell", "run-as", PACKAGE, "touch", "files/reframework/"+marker+".request")
    elif args.action == "stage-mod":
        entry = args.entry.resolve()
        if not entry.is_file() or entry.suffix.lower() != ".lua":
            p.error("Expected the PC mod's .lua entry file")
        directory = entry.with_suffix("")
        files = sorted(directory.rglob("*.lua")) if directory.is_dir() else []
        for file in [*files, entry]:
            stage(file, "files/reframework/autorun/" + file.relative_to(entry.parent).as_posix())
        print(f"Staged {len(files)+1} Lua files. Use reload to activate.")
    elif args.action in ("stage","stage-live"):
        name = Path(args.relative or args.script.name)
        if name.anchor or ".." in name.parts or name.suffix != ".lua":
            p.error("Expected a relative .lua destination")
        directory = "live" if args.action == "stage-live" else "autorun"
        dest = "files/reframework/" + directory + "/" + name.as_posix()
        stage(args.script,dest)
        print(dest)
    elif args.action == "probe":
        folder = "/data/local/tmp/mhrise-lua-" + uuid.uuid4().hex
        run("shell", "mkdir -p " + folder + "/autorun")
        try:
            run("push", ROOT/"build/tdb-android/nier_loader", folder+"/probe")
            run("push", args.script, folder+"/autorun/probe.lua")
            run("shell", "chmod 700 " + folder + "/probe && " + folder + "/probe " +
                shlex.quote(args.exefs) + " --tdb-autorun " + folder + "/autorun")
        finally:
            run("shell", "rm -rf " + folder)
    else:
        pid = run("shell", "pidof", PACKAGE, capture_output=True, text=True).stdout.strip()
        run("logcat", "--pid=" + pid.split()[0], "-d", "-s", "RiseLua")


if __name__ == "__main__":
    main()
