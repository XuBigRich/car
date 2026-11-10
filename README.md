# ESP32-S3 + TC1508A Wi-Fi 小车

新版板载录制、板端执行和技能导出见 [BOARD_MOTION.md](BOARD_MOTION.md)，需配套 App 2.0。

## 独立 GPIO 排查固件

测试前断开 TC1508A 的四根控制线。此模式不启动 Wi-Fi，也不使用 PWM。
上电先全部输出低电平 5 秒，随后 GPIO4 → GPIO5 → GPIO6 → GPIO7 依次输出高电平 2 秒，
每路之间全部拉低 1 秒，一轮后再全部拉低 3 秒，循环执行。串口 115200 输出当前步骤。
日志仅表示软件已执行设置，不等于外部电压已测量。

在 ESP-IDF 终端编译和烧录测试版本：

```sh
idf.py -B build-gpio-test -D SDKCONFIG=config/sdkconfig.gpio-test -D "SDKCONFIG_DEFAULTS=config/sdkconfig.defaults;config/sdkconfig.gpio-test.defaults" build
idf.py -B build-gpio-test -p COM4 flash monitor
```

此测试使用单独的配置和构建目录，原 Wi-Fi 控制程序保留。恢复 Wi-Fi 版本：

```sh
idf.py -B build -D SDKCONFIG=sdkconfig -D SDKCONFIG_DEFAULTS=config/sdkconfig.defaults build
idf.py -B build -p COM4 flash
```

适用 ESP-IDF 5.5，行走马达 A + 弹簧回正转向马达 B 底盘。手机直连 ESP32-S3 热点，使用安卓 App，无需路由器。

## 接线

| ESP32-S3 | TC1508A 模块 |
| --- | --- |
| GPIO4 | IN1 / INA（行走马达 A） |
| GPIO5 | IN2 / INB（行走马达 A） |
| GPIO6 | IN3 / INC（转向马达 B） |
| GPIO7 | IN4 / IND（转向马达 B） |
| GND | GND，与电机电源负极共地 |

行走马达接 OUTA/OUTB，转向马达接 OUTC/OUTD，以模块实际丝印为准。
模块电机电源使用适合电机的独立电源，ESP32 开发板通过 USB 或合适的稳压电源供电。
不要用 ESP32 的 3.3V 引脚为电机供电。
TC1508A 数据手册规定供电 2.4～7.2V；两节锂电池满电 8.4V，不能直接接入。
选型同时检查电机堵转电流及模块散热，数据手册的每通道 1.8A 连续/2.5A 峰值不代表模块可在任何条件下持续承受该电流。

## 编译与使用

在已激活的 ESP-IDF 5.5 终端中进入本项目：

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p COM端口号 flash monitor
```

也可在 VS Code 的 ESP-IDF 扩展中选择 ESP32-S3 后编译、烧录。

1. 首次测试将车轮架空，检查接线后上电。启动时电机保持停止。
2. 手机连接热点 **Control-Car**，密码 **car123456**。
3. 手机若提示热点无法上网，选择保持连接。
4. 安装并打开安卓横屏遥控器。
5. 默认行走速度 40%。前进/后退只驱动马达 A；左/右只驱动马达 B。支持双指同时按住行走和转向。松开某个按钮只释放该项控制，松开全部按钮停车并释放转向。转向靠机械弹簧回正，不进行位置闭环控制。
6. “刹车”仅使行走马达 A 的两输入保持高电平，同时释放转向马达 B；松开或点击“停止”解除刹车。刹车不是机械驻车装置。

App 约每 150ms 刷新控制信号；超过 800ms 未收到有效指令则自动滑行停车，Wi-Fi 客户端断开也会停车。App 进入后台后停止发送控制信号。默认仅允许一台手机接入。

## 修改配置

编辑 `main/include/car_config.h` 修改引脚、热点名称、密码（8～63 字符）、PWM 频率和停车超时。
若行走或转向方向相反，将对应的 `CAR_DRIVE_REVERSED` 或 `CAR_STEER_REVERSED` 改为 `1`，或断电后交换该电机两根线。
转向当前为按住持续满功率模式：启动和保持占空比均为 100%，`CAR_STEER_MAX_MS=0` 表示关闭转向持续时间限制。松开、停止、刹车和超过 800ms 未收到控制信号时均释放转向。左右切换前断电 100ms。到机械限位后会持续堵转，可能损伤马达或驱动，测试时应短按并观察，不能靠此模式保证堵转保护。
当前以 1kHz、10 位 PWM 调速，调速百分比表示占空比，非闭环车速；低占空比时电机可能无法起转。
切换电机正反转时先关闭输出并等待 30ms。根据实际电机惯性和负载调整测试参数。

## 文件

- `main/main.c`：启动入口；`main/drivers/motor.c`：电机和超时保护；`main/network/network.c`：热点和控制接口。
- `main/include/car_config.h`：接线和参数。

接口：`POST /api/drive`，正文如 `f,40`；方向 `f` 前进、`b` 后退、`l` 左转、`r` 右转、`s` 停止、`x` 刹车，速度 0～100，仅调节行走马达。组合方向 `q` 前进左转、`e` 前进右转、`z` 后退左转、`c` 后退右转。
控制接口供本地热点使用；持有热点密码的设备可控制小车，不应将接口转发至公网。

参考：[TC1508A 数据手册](https://www.chipsourcetek.com/DataSheet/TC1508A.pdf)。



固件使用一个应用组件。头文件统一放在 main/include/，驱动与网络实现分别放在 main/drivers/ 和 main/network/。config/ 保存构建配置，main/diagnostics/ 保存独立 GPIO 排查程序。


