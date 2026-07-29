# Beken CP 已验证文件

本目录保存已验证的 Beken CP driver.c完整文件。当前完整 driver.c 包含：CP shell 队列串行化、AP 日志按行缓存、ap0 前缀、50ms 半行刷新、队列提交失败 3 次重试。当前不 commit。

将此目录中"bk_avdk_smp/cp/middleware/driver/common/driver.c"替换bk_avdk_smp工程中原有driver.c文件。

覆盖前建议备份目标文件。
