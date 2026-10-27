#!/usr/bin/env python3
"""Named scenario harnesses for proof_probe.py transport tests."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


PROBE_SCRIPT = Path(__file__).with_name("proof_probe.py")
REPO_ROOT = PROBE_SCRIPT.parents[1]
WORKSPACE_ROOT = REPO_ROOT.parent
DEFAULT_RETICULUM_ROOT = WORKSPACE_ROOT / "Reticulum-master"
DEFAULT_TESTS_DIR = REPO_ROOT / "tests"


@dataclass(frozen=True)
class Scenario:
    name: str
    description: str
    client_side: str
    server_side: str
    work_dir_name: str
    default_payload: str


SCENARIOS = {
    "local-tcp-to-local-tcp": Scenario(
        name="local-tcp-to-local-tcp",
        description="Local TCP client to RTNode local TCP server.",
        client_side="tcp",
        server_side="tcp",
        work_dir_name="harness_local_tcp_to_local_tcp",
        default_payload="harness local tcp to local tcp",
    ),
    "lora-to-local-tcp": Scenario(
        name="lora-to-local-tcp",
        description="LoRa client to RTNode local TCP server.",
        client_side="lora",
        server_side="tcp",
        work_dir_name="harness_lora_to_local_tcp",
        default_payload="harness lora to local tcp",
    ),
    "local-tcp-to-wan": Scenario(
        name="local-tcp-to-wan",
        description="Local TCP client to RTNode WAN or LoRa server.",
        client_side="tcp",
        server_side="lora",
        work_dir_name="harness_local_tcp_to_wan",
        default_payload="harness local tcp to wan",
    ),
    "wan-to-local-tcp": Scenario(
        name="wan-to-local-tcp",
        description=(
            "WAN or LoRa client to RTNode local TCP server. Uses a separate work "
            "directory from lora-to-local-tcp for firewall tests."
        ),
        client_side="lora",
        server_side="tcp",
        work_dir_name="harness_wan_to_local_tcp",
        default_payload="harness wan to local tcp",
    ),
}

SCENARIO_SEQUENCE = [
    "local-tcp-to-local-tcp",
    "lora-to-local-tcp",
    "local-tcp-to-wan",
    "wan-to-local-tcp",
]


def quoted(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def scenario_work_dir(name: str) -> Path:
    return DEFAULT_TESTS_DIR / SCENARIOS[name].work_dir_name


def add_common_runtime_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--reticulum-root",
        type=Path,
        default=DEFAULT_RETICULUM_ROOT,
        help="Path to the Reticulum source tree used for PYTHONPATH.",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=None,
        help="Override the scenario work directory.",
    )
    parser.add_argument(
        "--python",
        default=sys.executable,
        help="Python interpreter used to launch proof_probe.py.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the command instead of running it.",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Pass through proof_probe.py debug logging.",
    )


def add_radio_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--rnode-port", default="/dev/cu.usbmodem114401")
    parser.add_argument("--frequency", type=int, default=914875000)
    parser.add_argument("--bandwidth", type=int, default=125000)
    parser.add_argument("--spreadingfactor", type=int, default=10)
    parser.add_argument("--codingrate", type=int, default=5)
    parser.add_argument("--txpower", type=int, default=14)


def add_tcp_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--tcp-host", default="mynode.local")
    parser.add_argument("--tcp-port", type=int, default=4242)


def add_server_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("scenario", choices=sorted(SCENARIOS))
    parser.add_argument("--duration", type=float, default=120)
    parser.add_argument("--announce-interval", type=float, default=15)
    add_common_runtime_arguments(parser)
    add_tcp_arguments(parser)
    add_radio_arguments(parser)


def add_client_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("scenario", choices=sorted(SCENARIOS))
    parser.add_argument("--path-timeout", type=float, default=60)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--payload", default=None)
    parser.add_argument("--target", default=None)
    add_common_runtime_arguments(parser)
    add_tcp_arguments(parser)
    add_radio_arguments(parser)


def add_orchestrator_arguments(parser: argparse.ArgumentParser, include_scenario: bool) -> None:
    if include_scenario:
        parser.add_argument("scenario", choices=sorted(SCENARIOS))
    parser.add_argument("--duration", type=float, default=120)
    parser.add_argument("--announce-interval", type=float, default=15)
    parser.add_argument("--path-timeout", type=float, default=60)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--payload", default=None)
    parser.add_argument("--target", default=None)
    parser.add_argument(
        "--server-ready-timeout",
        type=float,
        default=30,
        help="Maximum time to wait for the server hash file before starting the client.",
    )
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=2.0,
        help="Extra delay after server readiness before the client starts.",
    )
    parser.add_argument(
        "--keep-state",
        action="store_true",
        help="Keep old config and hash files in the scenario work directory.",
    )
    add_common_runtime_arguments(parser)
    add_tcp_arguments(parser)
    add_radio_arguments(parser)


def build_base_command(args: argparse.Namespace, role: str, side: str, work_dir: Path) -> list[str]:
    command = [
        args.python,
        str(PROBE_SCRIPT),
        role,
        "--side",
        side,
        "--work-dir",
        str(work_dir),
        "--hash-file",
        str(work_dir / "server_hash.txt"),
    ]

    if side == "tcp":
        command += ["--tcp-host", args.tcp_host, "--tcp-port", str(args.tcp_port)]
    elif side == "lora":
        command += [
            "--rnode-port",
            args.rnode_port,
            "--frequency",
            str(args.frequency),
            "--bandwidth",
            str(args.bandwidth),
            "--spreadingfactor",
            str(args.spreadingfactor),
            "--codingrate",
            str(args.codingrate),
            "--txpower",
            str(args.txpower),
        ]
    else:
        raise ValueError(f"Unsupported side: {side}")

    if args.debug:
        command.append("--debug")

    return command


def build_server_command(args: argparse.Namespace) -> list[str]:
    scenario = SCENARIOS[args.scenario]
    work_dir = args.work_dir or scenario_work_dir(args.scenario)
    command = build_base_command(args, "server", scenario.server_side, work_dir)
    command += [
        "--duration",
        str(args.duration),
        "--announce-interval",
        str(args.announce_interval),
    ]
    return command


def build_client_command(args: argparse.Namespace) -> list[str]:
    scenario = SCENARIOS[args.scenario]
    work_dir = args.work_dir or scenario_work_dir(args.scenario)
    command = build_base_command(args, "client", scenario.client_side, work_dir)
    command += [
        "--path-timeout",
        str(args.path_timeout),
        "--timeout",
        str(args.timeout),
        "--payload",
        args.payload or scenario.default_payload,
    ]
    if args.target:
        command += ["--target", args.target]
    return command


def run_with_environment(command: list[str], reticulum_root: Path, dry_run: bool) -> int:
    env = build_environment(reticulum_root)

    print(quoted(command))
    if dry_run:
        return 0

    completed = subprocess.run(command, env=env)
    return completed.returncode


def build_environment(reticulum_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    existing_pythonpath = env.get("PYTHONPATH")
    reticulum_entry = str(reticulum_root)
    if existing_pythonpath:
        env["PYTHONPATH"] = f"{reticulum_entry}{os.pathsep}{existing_pythonpath}"
    else:
        env["PYTHONPATH"] = reticulum_entry
    return env


def clean_runtime_state(work_dir: Path) -> None:
    for child in work_dir.iterdir() if work_dir.exists() else []:
        if child.is_dir() and child.name.startswith("config_"):
            shutil.rmtree(child)
        elif child.is_file() and child.name == "server_hash.txt":
            child.unlink()


def log_path(work_dir: Path, scenario_name: str, role: str) -> Path:
    safe_name = scenario_name.replace("-", "_")
    return work_dir / f"orchestrator_{safe_name}_{role}.log"


def append_header(log_file: Path, title: str, command: list[str]) -> None:
    log_file.parent.mkdir(parents=True, exist_ok=True)
    with log_file.open("w", encoding="utf-8") as handle:
        handle.write(f"[{title}]\n")
        handle.write(f"command: {quoted(command)}\n\n")


def append_output(log_file: Path, text: str) -> None:
    with log_file.open("a", encoding="utf-8") as handle:
        handle.write(text)
        if text and not text.endswith("\n"):
            handle.write("\n")


def tail_lines(log_file: Path, count: int = 20) -> list[str]:
    if not log_file.exists():
        return []
    lines = log_file.read_text(encoding="utf-8", errors="replace").splitlines()
    return lines[-count:]


def extract_signal_lines(log_file: Path) -> list[str]:
    if not log_file.exists():
        return []
    interesting = (
        "SERVER_READY",
        "SERVER_RX",
        "SERVER_DONE",
        "CLIENT_PATH_READY",
        "CLIENT_SENT",
        "CLIENT_DELIVERED",
        "CLIENT_TIMEOUT",
        "CLIENT_FAIL",
    )
    lines = log_file.read_text(encoding="utf-8", errors="replace").splitlines()
    return [line for line in lines if any(token in line for token in interesting)]


def wait_for_server_ready(
    server_process: subprocess.Popen[str],
    hash_file: Path,
    timeout_seconds: float,
) -> tuple[bool, str]:
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        if server_process.poll() is not None:
            return False, f"server exited with code {server_process.returncode} before readiness"
        if hash_file.exists() and hash_file.read_text(encoding="utf-8").strip():
            return True, "hash file ready"
        time.sleep(0.25)
    return False, f"server hash file not created within {timeout_seconds}s"


def stop_server(server_process: subprocess.Popen[str]) -> None:
    if server_process.poll() is not None:
        return
    server_process.terminate()
    try:
        server_process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server_process.kill()
        server_process.wait(timeout=10)


def run_scenario(args: argparse.Namespace, scenario_name: str) -> int:
    scenario = SCENARIOS[scenario_name]
    work_dir = args.work_dir or scenario_work_dir(scenario_name)
    hash_file = work_dir / "server_hash.txt"
    server_log = log_path(work_dir, scenario_name, "server")
    client_log = log_path(work_dir, scenario_name, "client")
    env = build_environment(args.reticulum_root)

    if not args.keep_state:
        clean_runtime_state(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)

    scenario_args = argparse.Namespace(**vars(args))
    scenario_args.scenario = scenario_name
    server_command = build_server_command(scenario_args)
    client_command = build_client_command(scenario_args)

    print(f"== {scenario_name} ==")
    print(f"server log: {server_log}")
    print(f"client log: {client_log}")

    if args.dry_run:
        print("Server command:")
        print(quoted(server_command))
        print("Client command:")
        print(quoted(client_command))
        return 0

    append_header(server_log, f"server {scenario_name}", server_command)
    append_header(client_log, f"client {scenario_name}", client_command)

    with server_log.open("a", encoding="utf-8") as server_handle:
        server_process = subprocess.Popen(
            server_command,
            env=env,
            stdout=server_handle,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=str(REPO_ROOT),
        )

    try:
        ready, reason = wait_for_server_ready(server_process, hash_file, args.server_ready_timeout)
        print(f"server readiness: {reason}")
        if not ready:
            print("server tail:")
            for line in tail_lines(server_log):
                print(line)
            return 90

        if args.settle_seconds > 0:
            time.sleep(args.settle_seconds)

        client_result = subprocess.run(
            client_command,
            env=env,
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )
        append_output(client_log, client_result.stdout)
        append_output(client_log, client_result.stderr)

        signal_lines = extract_signal_lines(client_log)
        if signal_lines:
            print("client signals:")
            for line in signal_lines:
                print(line)

        server_signals = extract_signal_lines(server_log)
        if server_signals:
            print("server signals:")
            for line in server_signals[-6:]:
                print(line)

        if client_result.returncode != 0:
            print(f"client failed with exit code {client_result.returncode}")
            print("client tail:")
            for line in tail_lines(client_log):
                print(line)
            print("server tail:")
            for line in tail_lines(server_log):
                print(line)
        else:
            print(f"scenario passed with client exit code {client_result.returncode}")

        return client_result.returncode
    finally:
        stop_server(server_process)


def command_list(_args: argparse.Namespace) -> int:
    for scenario in SCENARIOS.values():
        print(f"{scenario.name}: {scenario.description}")
        print(f"  server side: {scenario.server_side}")
        print(f"  client side: {scenario.client_side}")
        print(f"  default work dir: {scenario_work_dir(scenario.name)}")
    return 0


def command_show(args: argparse.Namespace) -> int:
    scenario = SCENARIOS[args.scenario]
    work_dir = args.work_dir or scenario_work_dir(args.scenario)
    print(f"Scenario: {scenario.name}")
    print(scenario.description)
    print(f"Work dir: {work_dir}")
    print()
    print("Server command:")
    print(quoted(build_server_command(args)))
    print()
    print("Client command:")
    print(quoted(build_client_command(args)))
    return 0


def command_server(args: argparse.Namespace) -> int:
    if not args.reticulum_root.exists():
        raise SystemExit(f"Reticulum root does not exist: {args.reticulum_root}")
    return run_with_environment(build_server_command(args), args.reticulum_root, args.dry_run)


def command_client(args: argparse.Namespace) -> int:
    if not args.reticulum_root.exists():
        raise SystemExit(f"Reticulum root does not exist: {args.reticulum_root}")
    return run_with_environment(build_client_command(args), args.reticulum_root, args.dry_run)


def command_run(args: argparse.Namespace) -> int:
    if not args.reticulum_root.exists():
        raise SystemExit(f"Reticulum root does not exist: {args.reticulum_root}")
    return run_scenario(args, args.scenario)


def command_run_all(args: argparse.Namespace) -> int:
    if not args.reticulum_root.exists():
        raise SystemExit(f"Reticulum root does not exist: {args.reticulum_root}")

    results: list[tuple[str, int]] = []
    for scenario_name in SCENARIO_SEQUENCE:
        scenario_args = argparse.Namespace(**vars(args))
        scenario_args.work_dir = None
        result = run_scenario(scenario_args, scenario_name)
        results.append((scenario_name, result))

    print("== summary ==")
    for scenario_name, result in results:
        status = "PASS" if result == 0 else f"FAIL ({result})"
        print(f"{scenario_name}: {status}")

    return 0 if all(result == 0 for _, result in results) else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    list_parser = subparsers.add_parser("list", help="List the named proof-probe scenarios.")
    list_parser.set_defaults(func=command_list)

    show_parser = subparsers.add_parser("show", help="Print the server and client commands for a scenario.")
    show_parser.add_argument("scenario", choices=sorted(SCENARIOS))
    show_parser.add_argument("--work-dir", type=Path, default=None)
    show_parser.add_argument("--duration", type=float, default=120)
    show_parser.add_argument("--announce-interval", type=float, default=15)
    show_parser.add_argument("--path-timeout", type=float, default=60)
    show_parser.add_argument("--timeout", type=float, default=45)
    show_parser.add_argument("--payload", default=None)
    show_parser.add_argument("--target", default=None)
    show_parser.add_argument("--python", default=sys.executable)
    show_parser.add_argument("--debug", action="store_true")
    add_tcp_arguments(show_parser)
    add_radio_arguments(show_parser)
    show_parser.set_defaults(func=command_show)

    server_parser = subparsers.add_parser("server", help="Run or print the server side of a scenario.")
    add_server_arguments(server_parser)
    server_parser.set_defaults(func=command_server)

    client_parser = subparsers.add_parser("client", help="Run or print the client side of a scenario.")
    add_client_arguments(client_parser)
    client_parser.set_defaults(func=command_client)

    run_parser = subparsers.add_parser("run", help="Run a full scenario: start server, then run client, then stop server.")
    add_orchestrator_arguments(run_parser, include_scenario=True)
    run_parser.set_defaults(func=command_run)

    run_all_parser = subparsers.add_parser("run-all", help="Run all four named scenarios sequentially.")
    add_orchestrator_arguments(run_all_parser, include_scenario=False)
    run_all_parser.set_defaults(func=command_run_all)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())