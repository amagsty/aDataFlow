串口查看器。README正在编写中。

[刷机教程（By Justin）](doc/manual/how_to_upload_firmware_BY_Justin.md)


## 自动构建与 Release

已发布的固件见 [Releases](https://github.com/niu541412/aDataFlow/releases)。下载名称带 `-full.bin` 的完整镜像，烧录地址为 **`0x0000`**（ESP32-S3 N8R2，DIO、80 MHz、8 MB Flash）。该镜像已合并引导程序、分区表和应用，可能重置原有设置。附件 `SHA256SUMS.txt` 可用于校验。

GitHub Actions 会在推送到 `main` 或推送 `v*` 标签时构建。发布新版本只需：

1. 修改 `include/main.h` 中的 `FIRMWARE_VERSION`，例如 `1.3.1`。
2. 在 `CHANGELOG.md` 中添加同版本的更新记录。
3. 提交并推送到 `main`，工作流会创建 `v1.3.1` 标签和 Release，上传从 `0x0000` 烧录的完整固件。也可自行推送同名标签，但标签版本必须与头文件一致。

已有的公开 Release 不会被覆盖；版本号未变的提交只生成构建产物。手动运行工作流也只构建，不发布。默认的 `GITHUB_TOKEN` 已在 workflow 中声明 `contents: write`，不需要另配个人令牌。

板卡定义直接使用项目内 `others/Boards`，无需复制到个人目录。已锁定当前验证过的框架和库版本。

本地构建、边界检查与合并：

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install platformio==6.2.0
pio run -e esp32s3dev_n8r2
python scripts/run_host_checks.py --ui-layout
python scripts/firmware_release.py package
```

完整镜像和校验文件输出到 `dist/`。存在未提交改动时，镜像名称带 `-local`，用于区分本地测试版和已发布固件。边界检查需要 `clang++` 或 `g++`；`--ui-layout` 还需要 CMake，用于链接实际 LVGL 和字体检查图例布局。只构建固件和合并镜像不需要这些主机测试工具。


## 图表配置行长度

1.3.1 起允许一行包含最多 **2048 字节有效内容**，CR/LF 换行不占该额度。这是软件缓冲限制，可在 `include/uart.h` 中调整；队列使用固定内存预算，增大单行上限会减少可排队的记录数。已发布的 1.3.0 仍使用旧的 352 字节限制。
