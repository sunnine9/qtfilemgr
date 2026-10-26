# QtFileMgr

可嵌入现有 Qt Widgets 程序的文件管理组件，同时提供多标签、双栏独立应用；
SSH/SFTP 后端可选。界面目前以中文为主。

![界面示例](docs/images/overview.png)

## 编译

Linux 为当前验证平台，需要 CMake 3.16+、C++17 编译器及 Qt Core/Gui/Widgets。
Qt 5 最低源码基线为 5.12，CI 覆盖 Qt5/Qt6；本次实际版本和结果见
[发布验证记录](docs/VALIDATION.md)。Windows/macOS 暂未承诺支持。

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build python3 qtbase5-dev
cmake -S . -B build -G Ninja -DQTFILEMGR_QT_MAJOR=5 \
  -DQTFILEMGR_WITH_SSH=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
(cd build && ctest --output-on-failure)
./build/qtfilemgr_app
```

Qt6 使用 `qt6-base-dev` 和 `-DQTFILEMGR_QT_MAJOR=6`。
启用 SSH 时安装 `pkg-config libssh-dev`，设置 `-DQTFILEMGR_WITH_SSH=ON`。
关闭后不编译 SSH 后端、不链接 libssh，也不显示内置连接入口；自定义 IO
和通用协议扩展接口继续可用。测试为 offscreen，并隔离用户设置与数据。

## 集成

`cmake --install build --prefix "$PWD/install"` 安装库、公开头文件和 CMake 包。
其他项目使用 `find_package(QtFileMgr CONFIG REQUIRED)` 并链接
`QtFileMgr::QtFileMgr`，可直接创建 `QtFileMgrWidget`，通过 `fileActivated`、
`filesSelected` 和 `urlChanged` 接收交互。完整的小型消费者构建见
[tests/installed_consumer](tests/installed_consumer)。

已提供[两个完整示例](docs/EXAMPLES.md)：嵌入现有窗口并联动宿主文本预览，
以及只读 `memory:///` 内存 IO。配置时增加 `-DQTFILEMGR_BUILD_EXAMPLES=ON`，
运行 `build/examples/embedded/qtfilemgr_example_embedded` 或
`build/examples/memory_io/qtfilemgr_example_memory_io`。两者也可独立通过
`find_package` 使用已安装的库构建，支持行为自检和截图。

CMake 是受支持的构建入口，仅需上方列出的公开依赖。程序使用独立的
QtFileMgr 配置命名空间，JSON 位于标准配置目录的
`qtfilemgr/mgr_default_config.json`，可用 `QTFILEMGR_CONFIG_FILE` 指定。

撤销现在校验操作完成时的对象身份和内容，已修改或无法验证的项目会保留。
远端覆盖先备份旧目标，提交失败后安全恢复；内置 SSH 成功覆盖后保留旧备份并
报告路径。校验容量限制及恢复步骤见[安全行为说明](docs/SAFETY.md)。

## 文档与许可

作者及维护者：**Spike**。Copyright (c) 2026 Spike，采用 MIT 协议。

- [英文主说明和构建选项](README.md)
- [测试、真实 SSH fixture 和安装验证](docs/TESTING.md)
- [已知限制和恢复边界](docs/KNOWN_LIMITATIONS.md)
- [发布检查表](docs/RELEASE_CHECKLIST.md)
- [MIT 许可证](LICENSE) 与 [代码来源、第三方依赖说明](NOTICE.md)

复制/移动的取消是协作式，覆盖和撤销并非版本备份，`allowedPaths` 不是安全沙箱。
SSH 保存密码采用混淆存储，不能替代操作系统密钥环。完整限制见上方文档。
