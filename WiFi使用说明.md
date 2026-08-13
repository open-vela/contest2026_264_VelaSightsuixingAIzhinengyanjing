# Wi-Fi 使用说明

当前固件使用手工配网。进入 AP 的 NSH 后执行：

```sh
ifup wlan0
wapi psk wlan0 <密码> 3
wapi essid wlan0 <2.4G_SSID> 1
renew wlan0
ping -c 4 8.8.8.8
```

`renew wlan0` 用于通过 DHCP 获取热点分配的 IP、掩码和默认网关，不能省略。

检查连接状态：

```sh
ifconfig wlan0
```

接口显示 `RUNNING` 且 `ping` 收到回复，表示 Wi-Fi 连接和公网访问正常。

不要把真实 Wi-Fi 密码提交到仓库或测试日志中。
