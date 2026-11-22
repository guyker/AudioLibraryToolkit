"""Verify first-run Unicode path entry through a real Windows pseudoconsole."""

import ctypes
import json
import os
import sys
import tempfile
import threading
import time
from ctypes import wintypes
from pathlib import Path

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
STARTF_USESTDHANDLES = 0x00000100
PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
CREATE_UNICODE_ENVIRONMENT = 0x00000400
EXTENDED_STARTUPINFO_PRESENT = 0x00080000


class Coord(ctypes.Structure):
    _fields_ = [("x", ctypes.c_short), ("y", ctypes.c_short)]


class StartupInfo(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD), ("reserved", wintypes.LPWSTR),
        ("desktop", wintypes.LPWSTR), ("title", wintypes.LPWSTR),
        ("x", wintypes.DWORD), ("y", wintypes.DWORD), ("xsize", wintypes.DWORD),
        ("ysize", wintypes.DWORD), ("xchars", wintypes.DWORD), ("ychars", wintypes.DWORD),
        ("fill", wintypes.DWORD), ("flags", wintypes.DWORD), ("show", wintypes.WORD),
        ("reserved_size", wintypes.WORD), ("reserved_data", ctypes.POINTER(ctypes.c_ubyte)),
        ("stdin", wintypes.HANDLE), ("stdout", wintypes.HANDLE), ("stderr", wintypes.HANDLE),
    ]


class StartupInfoEx(ctypes.Structure):
    _fields_ = [("startup", StartupInfo), ("attributes", ctypes.c_void_p)]


class ProcessInfo(ctypes.Structure):
    _fields_ = [("process", wintypes.HANDLE), ("thread", wintypes.HANDLE),
                ("pid", wintypes.DWORD), ("tid", wintypes.DWORD)]


kernel.CreatePipe.argtypes = [ctypes.POINTER(wintypes.HANDLE), ctypes.POINTER(wintypes.HANDLE),
                              ctypes.c_void_p, wintypes.DWORD]
kernel.CreatePipe.restype = wintypes.BOOL
kernel.CreatePseudoConsole.argtypes = [Coord, wintypes.HANDLE, wintypes.HANDLE, wintypes.DWORD,
                                       ctypes.POINTER(wintypes.HANDLE)]
kernel.CreatePseudoConsole.restype = ctypes.c_long
kernel.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                                      ctypes.POINTER(ctypes.c_size_t)]
kernel.InitializeProcThreadAttributeList.restype = wintypes.BOOL
kernel.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t, ctypes.c_void_p,
                                            ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
kernel.UpdateProcThreadAttribute.restype = wintypes.BOOL
kernel.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                  wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                  ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo)]
kernel.CreateProcessW.restype = wintypes.BOOL
kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel.ReadFile.restype = wintypes.BOOL
kernel.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                             ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel.WriteFile.restype = wintypes.BOOL
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel.ClosePseudoConsole.argtypes = [wintypes.HANDLE]
kernel.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]


def check(result):
    if not result:
        raise ctypes.WinError(ctypes.get_last_error())


def pipe():
    read, write = wintypes.HANDLE(), wintypes.HANDLE()
    check(kernel.CreatePipe(ctypes.byref(read), ctypes.byref(write), None, 0))
    return read, write


def main(interrupted=False):
    converter = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="alt-console-") as directory:
        root = Path(directory)
        (root / "input \u97f3\U0001f3b5").mkdir()
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(root / "user")
        block = ctypes.create_unicode_buffer(
            "\0".join(f"{key}={value}" for key, value in sorted(env.items())) + "\0\0")

        input_read, input_write = pipe()
        output_read, output_write = pipe()
        console = wintypes.HANDLE()
        result = kernel.CreatePseudoConsole(Coord(120, 40), input_read, output_write, 0, ctypes.byref(console))
        if result != 0:
            raise OSError(f"CreatePseudoConsole failed: HRESULT {result:#x}")
        size = ctypes.c_size_t()
        kernel.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
        attributes = ctypes.create_string_buffer(size.value)
        check(kernel.InitializeProcThreadAttributeList(attributes, 1, 0, ctypes.byref(size)))
        check(kernel.UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                                console, ctypes.sizeof(console), None, None))
        startup = StartupInfoEx()
        startup.startup.cb = ctypes.sizeof(startup)
        # Do not inherit the test runner's redirected standard handles.
        startup.startup.flags = STARTF_USESTDHANDLES
        startup.attributes = ctypes.addressof(attributes)
        process = ProcessInfo()
        command = ctypes.create_unicode_buffer(f'"{converter}"')
        check(kernel.CreateProcessW(str(converter), command, None, None, False,
                                    CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                    ctypes.cast(block, ctypes.c_void_p), str(root),
                                    ctypes.byref(startup.startup), ctypes.byref(process)))
        kernel.CloseHandle(input_read)
        kernel.CloseHandle(output_write)

        output = bytearray()

        def drain():
            buffer = ctypes.create_string_buffer(4096)
            count = wintypes.DWORD()
            while kernel.ReadFile(output_read, buffer, len(buffer), ctypes.byref(count), None) and count.value:
                output.extend(buffer.raw[:count.value])

        reader = threading.Thread(target=drain, daemon=True)
        reader.start()

        def wait_for(prompt):
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if prompt.encode() in output:
                    return
                if kernel.WaitForSingleObject(process.process, 0) == 0:
                    break
                time.sleep(.03)
            raise AssertionError(f"Missing prompt {prompt!r}: {output.decode('utf-8', 'replace')}")

        def send(text):
            data = text.encode("utf-8")
            written = wintypes.DWORD()
            check(kernel.WriteFile(input_write, data, len(data), ctypes.byref(written), None))
            assert written.value == len(data)

        try:
            wait_for("Output directory:")
            if interrupted:
                send("\x1a\r")
            else:
                send("output \u00e9\r")
                wait_for("Input library directory:")
                send(".\\" * 130 + "input \u97f3\U0001f3b5\\\r")
                wait_for("Enter option")
                send("0\r")
            assert kernel.WaitForSingleObject(process.process, 15000) == 0, "Application did not exit"
            exit_code = wintypes.DWORD()
            check(kernel.GetExitCodeProcess(process.process, ctypes.byref(exit_code)))
            assert (exit_code.value != 0) == interrupted, output.decode("utf-8", "replace")
            config_text = (root / "user" / "AudioLibraryToolkit" / "config.json").read_text(encoding="utf-8")
            assert "//" in config_text, "First run did not copy the commented default template"
            config = json.loads("\n".join(
                line for line in config_text.splitlines() if not line.lstrip().startswith("//")))
            if interrupted:
                assert config["OutDirectory"] == "" and config["MediaDirectoryList"] == [], config
            else:
                context = (ascii(config), ascii(output.decode("utf-8", "replace")))
                assert config["OutDirectory"] == str(root / "output \u00e9"), context
                assert config["MediaDirectoryList"][0]["mediaPath"] == str(root / "input \u97f3\U0001f3b5"), context
        finally:
            kernel.CloseHandle(input_write)
            kernel.WaitForSingleObject(process.process, 5000)
            kernel.ClosePseudoConsole(console)
            reader.join(timeout=5)
            kernel.CloseHandle(output_read)
            kernel.CloseHandle(process.thread)
            kernel.CloseHandle(process.process)
            kernel.DeleteProcThreadAttributeList(attributes)


if __name__ == "__main__":
    main()
    main(interrupted=True)
