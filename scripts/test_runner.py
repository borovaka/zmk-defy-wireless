# SPDX-License-Identifier: MIT
"""Running test commands with logs, timeouts and a summary, on several threads."""

from concurrent.futures import ThreadPoolExecutor, wait
from contextlib import nullcontext
from pathlib import Path
import os
import signal
import subprocess
import sys
import threading


# Commands may run on several threads. Each report is written whole under
# _output_lock so lines never split, and _running maps each command still
# running to whether stop_commands() has killed it.
_output_lock = threading.Lock()
_running_lock = threading.Lock()
_running = {}


def report(*lines, file=None):
    text = "".join(f"{line}\n" for line in lines)
    with _output_lock:
        stream = sys.stdout if file is None else file
        stream.write(text)
        stream.flush()


def failure_log(path):
    if path is None:
        return []
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError as error:
        return [f"Log: {path}", f"Cannot read log: {error}"]
    return [f"Log: {path}", *lines[-40:]]


def kill_group(process):
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def stop_commands():
    """Kill every running command; each then fails without a report."""
    with _running_lock:
        for process in _running:
            if process.returncode is None:
                kill_group(process)
            _running[process] = True


def run_command(label, command, *, cwd, timeout, log_path=None):
    report(f"Running {label}")
    failure = None
    stopped = False
    try:
        if log_path is not None:
            log_path = Path(log_path)
            log_path.parent.mkdir(parents=True, exist_ok=True)
        output = log_path.open("w") if log_path is not None else nullcontext(None)
        with output as log:
            process = subprocess.Popen(
                command, cwd=cwd, stdout=log,
                stderr=subprocess.STDOUT if log is not None else None,
                start_new_session=True,
            )
            with _running_lock:
                _running[process] = False
            try:
                result = process.wait(timeout=timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                kill_group(process)
                process.wait()
                if isinstance(error, KeyboardInterrupt):
                    raise
                failure = f"FAILED: {label} exceeded {timeout:g}s"
                result = -1
            finally:
                with _running_lock:
                    stopped = _running.pop(process)
    except OSError as error:
        report(f"FAILED: {label}: {error}", *failure_log(log_path), file=sys.stderr)
        return False
    if stopped:
        return False
    if result != 0:
        failure = failure or f"FAILED: {label} exited with status {result}"
        report(failure, *failure_log(log_path), file=sys.stderr)
        return False
    report(f"PASS: {label}")
    return True


def run_tasks(tasks, jobs):
    """Calls each task, up to jobs at once, and returns their results in order.

    An interrupt starts no more tasks and kills the commands still running,
    including any that a task starts while the threads wind down.
    """
    if jobs <= 1:
        return [task() for task in tasks]
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = [executor.submit(task) for task in tasks]
        try:
            return [future.result() for future in futures]
        except BaseException:
            executor.shutdown(wait=False, cancel_futures=True)
            pending = [future for future in futures if not future.cancelled()]
            while pending:
                stop_commands()
                pending = wait(pending, timeout=0.1).not_done
            raise


def summarize(results):
    """Reports how many results passed, and the label and log of each that failed.

    A result is None for a pass, otherwise the failed step's label and its log or
    None. Returns the exit status.
    """
    failures = [result for result in results if result]
    report(f"Summary: {len(results) - len(failures)} of {len(results)} passed",
           *(f"FAILED: {label}" + (f" (log: {log_path})" if log_path else "")
             for label, log_path in failures))
    return 1 if failures else 0
