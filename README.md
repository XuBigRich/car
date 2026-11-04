# ESP32-S3 + TC1508A Wi-Fi 小车

## 独立 GPIO 排查固件

测试前断开 TC1508A 的四根控制线。此模式不启动 Wi-Fi，也不使用 PWM。
上电先全部输出低电平 5 秒，随后 GPIO4 → GPIO5 → GPIO6 → GPIO7 依次输出高电平 2 秒，
每路之间全部拉低 1 秒，一轮后再全部拉低 3 秒，循环执行。串口 115200 输出当前步骤。
日志仅表示软件已执行设置，不等于外部电压已测量。

在 ESP-IDF 终端编译和烧录测试版本：

```sh
idf.py -B build-gpio-test -D SDKCONFIG=sdkconfig.gpio-test -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.gpio-test.defaults" build
idf.py -B build-gpio-test -p COM4 flash monitor
```

此测试使用单独的配置和构建目录，原 Wi-Fi 控制程序保留。恢复 Wi-Fi 版本：

```sh
idf.py -B build -D SDKCONFIG=sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build
idf.py -B build -p COM4 flash
```

适用 ESP-IDF 5.5，行走马达 A + 弹簧回正转向马达 B 底盘。手机直连 ESP32-S3 热点，无需路由器或手机 App。

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
4. 浏览器打开 **http://192.168.4.1**（使用 HTTP）。
5. 默认行走速度 40%。前进/后退只驱动马达 A；左/右只驱动马达 B。支持双指同时按住行走和转向。松开某个按钮只释放该项控制，松开全部按钮停车并释放转向。转向靠机械弹簧回正，不进行位置闭环控制。
6. “刹车”仅使行走马达 A 的两输入保持高电平，同时释放转向马达 B；点击“停止”解除刹车。刹车不是机械驻车装置。

页面每 150ms 刷新控制信号；超过 800ms 未收到有效指令则自动滑行停车，Wi-Fi 客户端断开也会停车。网页进入后台后停止发送控制信号。默认仅允许一台手机接入。

## 修改配置

编辑 `main/car_config.h` 修改引脚、热点名称、密码（8～63 字符）、PWM 频率和停车超时。
若行走或转向方向相反，将对应的 `CAR_DRIVE_REVERSED` 或 `CAR_STEER_REVERSED` 改为 `1`，或断电后交换该电机两根线。
转向当前为满功率短脉冲测试：每次按下通电 150ms，随后断电锁定，不再低功率保持；必须松开转向键才可重新触发，提前松开立即断电。参数集中在 `main/car_config.h`。左右直接切换不会重置总计时，换向前断电 100ms。网页心跳不会重置计时；后台任务每约 10ms 更新，实际脉冲长度存在任务调度误差。断电后由弹簧回正，不能持续保持转弯。此参数尚未实机标定，首次架空前轮单次短按观察，不要连续反复触发。
当前以 1kHz、10 位 PWM 调速，调速百分比表示占空比，非闭环车速；低占空比时电机可能无法起转。
切换电机正反转时先关闭输出并等待 30ms。根据实际电机惯性和负载调整测试参数。

## 文件

- `main/main.c`：电机 PWM、热点、网页接口和超时停车。
- `main/index.html`：手机触摸控制页面，随固件嵌入。
- `main/car_config.h`：接线和参数。

接口：`POST /api/drive`，正文如 `f,40`；方向 `f` 前进、`b` 后退、`l` 左转、`r` 右转、`s` 停止、`x` 刹车，速度 0～100，仅调节行走马达。组合方向 `q` 前进左转、`e` 前进右转、`z` 后退左转、`c` 后退右转。
控制接口供本地热点使用；持有热点密码的设备可控制小车，不应将接口转发至公网。

参考：[TC1508A 数据手册](https://www.chipsourcetek.com/DataSheet/TC1508A.pdf)。
