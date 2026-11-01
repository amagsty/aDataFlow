#!/usr/bin/env python3
"""Exercise actual legend creation and sizing against real LVGL and font metrics."""

from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    if not shutil.which("cmake"):
        raise RuntimeError("The real LVGL layout check needs CMake")
    lvgl = ROOT / ".pio/libdeps/esp32s3dev_n8r2/lvgl"
    source = (ROOT / "src/ui.cpp").read_text()
    temp = ROOT / ".pio/host-legend"
    temp.mkdir(parents=True, exist_ok=True)
    write_changed(temp / "ui_structs.inc", section(source, "struct chart_range\n", "static struct chart_range chart_ranges"))
    write_changed(temp / "legend_helpers.inc", section(source, "static char ui_legend_value_text[", "static void ui_update_chart_range("))
    write_changed(temp / "terminal_events.inc", section(source, "static void ui_event_panList(", "static void lv_my_disp_flush("))
    write_changed(temp / "terminal_helpers.inc", section(source, "static void ui_term_new_line(bool", "static void ui_star_shoot("))
    write_changed(temp / "legend_creation.inc", section(source, "    ui_panSeries = lv_obj_create(", "    ui_panWaiting = lv_obj_create("))
    write_changed(temp / "esp_timer.h", "#pragma once\n#include <stdint.h>\nstatic inline int64_t esp_timer_get_time(void) { return 0; }\n")
    write_changed(temp / "CMakeLists.txt", f'''
cmake_minimum_required(VERSION 3.16)
project(legend_check LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
file(GLOB_RECURSE LVGL_SOURCES "{lvgl}/src/*.c")
add_library(lvgl_host STATIC ${{LVGL_SOURCES}})
target_include_directories(lvgl_host PUBLIC "{lvgl}" "{ROOT}/others" "{ROOT}/include" "{temp}" "{ROOT}/.pio/libdeps/esp32s3dev_n8r2/ArduinoJson/src")
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(lvgl_host PRIVATE -fsanitize=address -fno-omit-frame-pointer)
add_executable(legend_check "{ROOT}/tests/host_legend.cpp"
"{ROOT}/src/fonts/ui_font_SFMono_23.c"
"{ROOT}/src/fonts/ui_font_SFMono_14.c"
"{ROOT}/src/fonts/ui_font_SFMono_9.c")
target_compile_options(legend_check PRIVATE -fsanitize=address -fno-omit-frame-pointer)
target_link_options(legend_check PRIVATE -fsanitize=address)
target_link_libraries(legend_check PRIVATE lvgl_host m)
''')
    log = temp / "build.log"
    with log.open("w") as output:
        configured = subprocess.run(["cmake", "-S", str(temp), "-B", str(temp / "build")], stdout=output, stderr=subprocess.STDOUT)
        if configured.returncode == 0:
            configured = subprocess.run(["cmake", "--build", str(temp / "build"), "--parallel", "4"], stdout=output, stderr=subprocess.STDOUT)
    if configured.returncode:
        print(log.read_text()[-8000:])
        raise RuntimeError("LVGL layout test did not build")
    subprocess.run([str(temp / "build/legend_check")], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
