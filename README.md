# 米家单键开关

**ESP32-C3** 伪装小米 BLE 单键无线开关（**XMWXKG01CM** / PID `0x2387`），
FSR 压力传感器按压 → 单击/长按事件 → 米家自动化控制台灯。

> 纯 BLE 方案。历史上的 WiFi 直连米家控灯链路已停用，WiFi/米家凭据子系统
> 已整体移除（代码与配置不入库）。

## 项目状态（2026-11-16 实测）

| 环节 | 状态 | 判据 |
|------|------|------|
| 米家 App 发现/绑定 | ✅ | `REGISTER COMPLETE (result 0x11 on 0x0010)` → `bound=1` |
| 单击事件上云 | ✅ | `0x4E0C` 加密广播 → 网关 → 自动化执行 `error:0` |
| 长按事件 | ✅ 代码就绪 | `0x4E0E`（按压 ≥1s） |
| 跨重启防重放 | ✅ | `frame_cnt` NVS 持久化（起点 100000，单调不回跳） |

## 硬件接线（FSR AO/DO 压感模块）

| 模块脚 | 信号 | 接 C3 |
|--------|------|-------|
| 1 | D0（数字，比较器输出） | **GPIO1** |
| 2 | A0（模拟） | **GPIO2**（ADC1_CH2） |
| 3 | VCC | 3V3 或 5V（模块灯亮 = 供电正常） |
| 4 | GND | GND |

判定：DO 主判据（空载=1，按压翻 0）+ AO 滞回（<2500 按下 / >3000 释放），采样 50ms。

## 构建烧录

需要 **ESP-IDF v5.5.x**（目标 `esp32c3`）。Windows 下：

```powershell
. E:\ESP\esp_env.ps1
idf.py build
idf.py -p COM36 flash          # 端口按实际改
```

- **首次烧录必须 `idf.py -p COM36 erase-flash`**：清掉残留 NVS 身份，否则串身份。
- 绑定成功后**只 flash 不擦除**，否则要重新走一遍米家添加。
- 本仓库 `sdkconfig` 不入库（`.gitignore`），`sdkconfig.defaults` 已含
  NimBLE + mbedTLS(CCM/ECDH/SECP256R1) 必需项，clone 后可直接构建。

## 工作原理

```
FSR 按压 ──按下沿──▶ fire_button(0x4E0C/0x4E0E)
                        │  未绑定：hello 帧（FC 30 2A，solicited=1）唤醒 App 连接
                        │  已绑定：v5 nomac 加密帧（bind_key + frame_cnt）
                        ▼
              2.5s 快广播(48~80ms) ──▶ 米家 BLE 网关 ──▶ 云端 ──▶ 自动化开/关灯
                        │
              到期回退 0x100A 电量心跳（800ms 保活）
```

- **广播**：绑定前 `miot_beacon_frame_hello`（FC 高字节强制 `0x2A` = solicited）；
  绑定后 `miot_beacon_frame_v5_nomac`（无 MAC 形态，31B 预算内放得下对象+MIC）。
- **GATT**：`0xFE95` 服务 + 四特征 `0x0010/0x0019/0x001A/0x001B`
  （写入按认证命令白名单分流；notify 出口：auth→0010/0019，spec→0010）。
- **身份**：`did/bind_key` 由云端 REGISTER 下发，NVS namespace `pmjbtn` 持久化
  （与 `fcnt` 同库）；帧内 MAC 取 eFuse 并转 LSB 序参与 nonce。

## 排障要点（实测踩坑，先查这里）

1. **App 点添加后 30s「连接超时」、串口零 `GAP CONNECT`**
   → 未绑定 hello 帧必须带 **solicited 位（FC 高字节 `0x2A`）**，缺位 App 永不发起连接。
2. **串口在发 `event 0x4E0C`，米家设备日志/自动化零记录**
   → 先查米家自动化场景 **enable 状态**（`miot_scene_info`：`enable:false` 时事件上云也不执行）；
   再查 **`frame_cnt` 是否回跳**（烧录 hard reset 会归 1，低于网关水位 → 重放丢弃，本项目已 NVS 持久化修复）。
3. **绑定失败 104 / 扫不到**
   → 广播须含 PID + `FC 30 2A`、adv ≤31B（超了自动去名重试）、GATT 四特征缺一不可。
4. **米家提示「按一下设备」**
   → 单击 FSR 即触发广播刷新（frame_cnt 跳变），等价真机按键。

## 仓库内容

- `main/` — 固件：FSR 采样、BLE 开关链（`ble_switch.c`）、NVS 持久化（`store.c`/`fcnt_store.c`）
- `miot-ble-sdk-main/` — 协议 SDK（vendored，来自 [MYHealer/miot-ble-sdk](https://github.com/MYHealer/miot-ble-sdk)，
  本地含 `miot_beacon_frame_v5_nomac` 补丁；上游的 `AGENTS.md`/`docs/` 是协议金标准，建议通读）
- `scripts/extract_credentials.ps1` — 已停用的 WiFi 凭据注入脚本（保留仅作历史参考，生成物不入库）

## 免责声明

1. 本项目为 **MiBeacon 协议研究 / 互操作性验证**用途，非可售产品（见 SDK `NOTICE.md`）。
2. 设备能被米家接受的前提是 **真实 PID + 国区账号 + 官方原版 App**，云端策略由小米控制，
   不承诺一定能添加成功。
3. 走 legacy PSK 的型号（驱蚊器等）SDK 已实现但未实机验证；请勿 root / 修改 / 注入米家 App。
