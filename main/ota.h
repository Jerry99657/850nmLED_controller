/*
 * 固件在线更新（HTTP OTA）
 *
 * ESP32-C3 没有 STM32 那种 USB DFU，想"不进下载模式"换固件只有一条路：让板子自己把
 * 新镜像写进另一块 app 分区再重启，也就是 OTA。分区表因此改成双 OTA（ota_0 / ota_1 各
 * 1700K，见 sdkconfig.defaults），第一次换表要用数据线整片烧一次，之后全靠网页。
 *
 * 协议（HTTP，端口 80）：
 *   POST /api/ota   Content-Type: application/octet-stream，body = app.bin 原文，
 *                   Content-Length = 镜像大小。成功回 {"ok":true,"ver":...} 后约 1.5 s 重启。
 *                   失败回 4xx/5xx + {"ok":false,"err":...,"code":esp_err_t}，板子继续跑旧固件。
 *   OPTIONS /api/ota  跨域预检。响应带 Access-Control-Allow-Origin: *，
 *                     所以 tools/fw_update.html 直接用 file:// 打开也能上传。
 *
 * 安全：没有任何鉴权，连上热点（WPA2，密码在"关于"页）的人就能刷固件。
 *      下面的 project_name 校验只防"传错文件"，不是鉴权。
 */
#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

// 注册到 http_start() 的 URI 表里
esp_err_t ota_http_post(httpd_req_t *req);
esp_err_t ota_http_options(httpd_req_t *req);

// 正在上传：remote.c 的链路管理别在这个节骨眼上关热点（板子只有一个 httpd 任务，
// 热点一停这次上传就断了）
bool ota_busy(void);

// 启动时打印跑在哪个分区、什么状态（OTA 之后回过滚没有，一眼能看出来）
void ota_report_boot(void);

// 当前运行的分区名（"ota_0" / "ota_1"）：/api/status 和更新页用它显示刷到哪一块了
const char *ota_running_partition_label(void);

// 主循环健康跑满一段时间后调用，确认这次启动；不确认就自动退回旧固件
void ota_confirm_boot(void);
