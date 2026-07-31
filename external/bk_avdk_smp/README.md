# Beken AVDK 覆盖文件

本目录保存 BK7258 OpenVela 最终固件构建所需的 Beken AVDK 覆盖文件。

覆盖关系：

```text
cp/middleware/driver/common/driver.c
  -> bk_avdk_smp/cp/middleware/driver/common/driver.c

projects/app_ab/partitions/bk7258/ram_regions.csv
  -> bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv
```

`driver.c`包含 CP shell 队列串行化、AP 日志按行缓存、`ap0:`来源前缀、
50 ms 半行刷新和队列提交失败有限重试。

`ram_regions.csv`采用原厂 16 MB 七区域布局：OpenVela AP 管理四个媒体
slab、2.875 MiB AP 动态 heap 和 6 MiB AP 静态 section；CP 保留
`0x60700000..0x6071ffff`的 128 KiB heap。

覆盖前建议备份目标文件。
