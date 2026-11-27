# 官方文档索引（ESP32-S3 移植相关）

本目录存放从官方文档摘录的、与本项目（在 ESP32-S3 上从零跑 c6os + WiFi）直接相关的段落。
摘录只保留关键结论和原文，全文请查官方 PDF。

## 一手文档（可下载）

| 文档 | 链接 | 用途 |
|---|---|---|
| ESP32-S3 Technical Reference Manual v1.8 | https://documentation.espressif.com/esp32-s3_technical_reference_manual_cn.pdf | 寄存器与架构，**唯一的寄存器权威** |
| ESP32-S3 Datasheet | https://documentation.espressif.com/api/resource/doc/file/rz94aWY3/FILE/esp32-s3_datasheet_en.pdf | 引脚/电气/内存映射概览 |
| IDF 应用镜像格式 | `esp-idf/docs/en/api-reference/system/app_image_format.rst` | 镜像头/段头格式（我们自己造镜像时用） |
| IDF 启动流程 | `esp-idf/docs/en/api-guides/startup.rst` | 三段式引导、bootloader 职责 |
| IDF WiFi API | `esp-idf/docs/zh_CN/api-reference/network/esp_wifi.rst` | 只有 API 层，闭源栈 |

本地 IDF 源码在 `/media/winger/inst_26/esp/esp-idf`（v6.0.1）。

## 问题报告

- `../support-report.md` —— 整理好的 Espressif 问题报告（背景/现象/环境/已排除 8 项/
  两边各自代码读的对照表/4 个具体问题），可直接发帖或提工单。

## 摘录文件

- `trm-cache-mmu.md` —— TRM §4.3.3 外部存储器 / 高速缓存 / Cache 操作
- `trm-pms.md` —— TRM §15 权限控制 (PMS)，**访问外部存储器必须获得 PMS 授权**
- `trm-system-cache.md` —— TRM §17 系统寄存器里的 cache 控制位
- `image-format.md` —— 我们实测出来的合法镜像格式（含原厂镜像逐字节对照）
