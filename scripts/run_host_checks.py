#!/usr/bin/env python3
"""Run boundary checks against the actual chart/startup code with device I/O stubbed."""

from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def main():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("Install clang++ or g++ to run host checks")
    json_headers = ROOT / ".pio/libdeps/esp32s3dev_n8r2/ArduinoJson/src"
    if not json_headers.exists():
        raise RuntimeError("Run pio run -e esp32s3dev_n8r2 to install ArduinoJson first")
    ui = (ROOT / "src/ui.cpp").read_text()
    baud = (ROOT / "src/bandrate.cpp").read_text()
    recording = (ROOT / "src/sdcard.cpp").read_text()
    uart = (ROOT / "src/uart.cpp").read_text()
    network = (ROOT / "src/network_time.cpp").read_text()
    uart_header = (ROOT / "include/uart.h").read_text()
    with tempfile.TemporaryDirectory(prefix="adataflow-host-") as temp:
        temp = Path(temp)
        (temp / "ui_structs.inc").write_text(section(ui, "struct chart_range\n", "static struct chart_range chart_ranges"))
        functions = section(ui, "static void ui_update_chart_range(", "static void ui_update_chart_color()")
        functions += section(ui, "static void ui_update_chart_config(", "static void ui_task_update_topbar(")
        functions += section(ui, "static void ui_startup_tick(", "static void ui_start_animations(")
        functions += baud[baud.index("void save_bandrate_to_nvs("):]
        (temp / "ui_functions.inc").write_text(functions)
        recording_functions = section(recording, "// Caller holds recording_mutex", "static void task_led_blink(")
        recording_functions += section(recording, "static bool prepare_recording_folder(", "void sdcard_init(")
        (temp / "recording_functions.inc").write_text(recording_functions)
        (temp / "uart_types.inc").write_text(section(uart_header, "#define UART_LINE_MAX_LENGTH", "extern QueueHandle_t"))
        (temp / "uart_functions.inc").write_text(section(uart, "typedef struct\n{\n    char data", "void onReceive_rx_cb()"))
        (temp / "network_functions.inc").write_text(section(network, "// All radio operations", "static void network_task("))
        wrapper = (ROOT / "src/smartconfig_task_stack.cpp").read_text()
        (temp / "task_stack_wrapper.inc").write_text(wrapper[wrapper.index('extern "C"'):])
        for name in ("host_safety", "host_recording", "host_uart", "host_network", "host_task_stack"):
            executable = temp / name
            command = [compiler, "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-I" + str(temp), "-I" + str(ROOT / "include"), "-I" + str(json_headers),
                       str(ROOT / f"tests/{name}.cpp"), "-o", str(executable)]
            subprocess.run(command, cwd=ROOT, check=True)
            subprocess.run([str(executable)], cwd=ROOT, check=True)
    suite = unittest.defaultTestLoader.discover(str(ROOT / "tests"), pattern="test_*.py")
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ui-layout", action="store_true", help="also check actual LVGL layout (needs CMake)")
    arguments = parser.parse_args()
    main()
    if arguments.ui_layout:
        from check_legend_layout import main as check_layout
        check_layout()
