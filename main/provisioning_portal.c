#include "provisioning_portal.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "wifi_manager.h"

static const char *TAG = "portal";
static httpd_handle_t s_server;
static TaskHandle_t s_dns_task;
static int s_dns_socket = -1;

static const char PAGE[] __attribute__((unused)) =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-S3 Wi-Fi 配置</title><style>"
"body{font-family:system-ui,sans-serif;background:#f3f5f7;margin:0;color:#18202a}"
"main{max-width:560px;margin:24px auto;padding:20px;background:white;border-radius:16px;box-shadow:0 4px 24px #0002}"
"h1{font-size:1.45rem;margin-top:0}label{display:block;margin-top:14px;font-weight:600}"
"select,input,button{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;border:1px solid #bcc5cf;border-radius:9px;font-size:1rem}"
"button{background:#1267d6;color:white;border:0;font-weight:700;cursor:pointer}button:disabled{opacity:.55}"
"#status{margin-top:16px;padding:12px;background:#eef4fb;border-radius:9px;white-space:pre-wrap}.hint{color:#5b6570;font-size:.9rem}"
"</style></head><body><main><h1>ESP32-S3 Wi-Fi 配置</h1>"
"<p class='hint'>选择附近的 2.4 GHz Wi-Fi，输入密码后设备会先验证连接，成功后才保存。</p>"
"<button id='scanBtn' type='button'>扫描附近 Wi-Fi</button>"
"<label>Wi-Fi 名称</label><select id='net' onchange='pick()'><option value=''>请先扫描</option></select>"
"<label>SSID（隐藏网络可手动填写）</label><input id='ssid' maxlength='32' autocomplete='off'>"
"<label>加密方式</label><select id='auth'>"
"<option value='0'>开放网络</option><option value='1'>WEP</option><option value='2'>WPA</option>"
"<option value='3' selected>WPA2</option><option value='4'>WPA/WPA2</option>"
"<option value='6'>WPA3</option><option value='7'>WPA2/WPA3</option></select>"
"<label>密码</label><input id='password' type='password' maxlength='63' autocomplete='new-password'>"
"<button id='connect' onclick='connectWifi()'>连接并保存</button><div id='status'>等待操作</div>"
"<script>let nets=[];const st=document.getElementById('status');"
"async function scanNetworks(){let b=document.getElementById('scanBtn');b.disabled=true;st.textContent='正在扫描…';"
"try{let r=await fetch('/api/scan');let j=await r.json();if(!r.ok)throw Error(j.message||'扫描失败');nets=j;let s=document.getElementById('net');s.innerHTML='<option value=\"\">请选择网络</option>';"
"nets.forEach((n,i)=>{let o=document.createElement('option');o.value=i;o.textContent=`${n.ssid||'(隐藏网络)'}  ${n.rssi} dBm  ${n.auth_name}`;s.appendChild(o)});st.textContent=`找到 ${nets.length} 个网络`;}"
"catch(e){st.textContent='扫描失败：'+e}finally{b.disabled=false}}"
"function pick(){let i=document.getElementById('net').value;if(i==='')return;let n=nets[+i];document.getElementById('ssid').value=n.ssid;document.getElementById('auth').value=n.auth;}"
"async function connectWifi(){let d=new URLSearchParams({ssid:document.getElementById('ssid').value,password:document.getElementById('password').value,auth:document.getElementById('auth').value});"
"document.getElementById('connect').disabled=true;st.textContent='正在测试连接…';try{let r=await fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d});let j=await r.json();if(!r.ok)throw Error(j.message||'提交失败');st.textContent=j.message;poll();}catch(e){st.textContent='错误：'+e.message;document.getElementById('connect').disabled=false}}"
"async function poll(){try{let r=await fetch('/api/status');let j=await r.json();st.textContent=j.message;if(j.state===2){st.textContent+='\n配置已保存，配网热点将在数秒后关闭。';return;}if(j.state===4)document.getElementById('connect').disabled=false;setTimeout(poll,1000);}catch(e){st.textContent='设备已切换到目标 Wi-Fi；可以关闭此页面。'}}"
"document.getElementById('scanBtn').addEventListener('click',scanNetworks);"
"</script></main></body></html>";

/* Keep the executable page ASCII-only so source/editor encodings cannot break JavaScript. */
static const char PAGE_SAFE[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-S3 Wi-Fi &#x914D;&#x7F51;</title><style>body{font-family:system-ui,sans-serif;background:#f3f5f7;margin:0;color:#18202a}main{max-width:560px;margin:24px auto;padding:20px;background:#fff;border-radius:16px}label{display:block;margin-top:14px;font-weight:600}select,input,button{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;border:1px solid #bcc5cf;border-radius:9px;font-size:1rem}button{background:#1267d6;color:#fff;border:0;font-weight:700}button:disabled{opacity:.55}#status{margin-top:16px;padding:12px;background:#eef4fb;border-radius:9px;white-space:pre-wrap}</style></head>"
"<body><main><h1>ESP32-S3 Wi-Fi &#x914D;&#x7F51;</h1><p>&#x8BF7;&#x9009;&#x62E9;&#x9644;&#x8FD1;&#x7684; 2.4 GHz Wi-Fi &#x7F51;&#x7EDC;&#x3002;</p>"
"<button id='scanBtn' type='button'>&#x626B;&#x63CF;&#x9644;&#x8FD1; Wi-Fi</button><label>Wi-Fi &#x7F51;&#x7EDC;</label><select id='net'><option value=''>&#x8BF7;&#x5148;&#x70B9;&#x51FB;&#x626B;&#x63CF;</option></select>"
"<label>SSID</label><input id='ssid' maxlength='32'><label>&#x52A0;&#x5BC6;&#x65B9;&#x5F0F;</label><select id='auth'><option value='0'>&#x5F00;&#x653E;&#x7F51;&#x7EDC;</option><option value='1'>WEP</option><option value='2'>WPA</option><option value='3' selected>WPA2</option><option value='4'>WPA/WPA2</option><option value='6'>WPA3</option><option value='7'>WPA2/WPA3</option></select>"
"<label>Wi-Fi &#x5BC6;&#x7801;</label><input id='password' type='password' maxlength='63'><label>esp32-status Token</label><input id='token' type='password' maxlength='128' placeholder='OpenWrt API Token'><label>&#x76D1;&#x63A7;&#x8BBE;&#x5907; IP</label><input id='monitorIp' inputmode='decimal' maxlength='15' placeholder='&#x7559;&#x7A7A;&#x5219;&#x4F7F;&#x7528;&#x9ED8;&#x8BA4;&#x7F51;&#x5173;'><button id='connectBtn' type='button'>&#x8FDE;&#x63A5;&#x5E76;&#x4FDD;&#x5B58;</button><div id='status'>&#x7B49;&#x5F85;&#x64CD;&#x4F5C;</div>"
"<script>var nets=[],st=document.getElementById('status');"
"function scanNetworks(){var b=document.getElementById('scanBtn');b.disabled=true;st.textContent='\u6b63\u5728\u626b\u63cf...';fetch('/api/scan',{cache:'no-store'}).then(function(r){return r.json().then(function(j){if(!r.ok)throw Error();return j})}).then(function(j){nets=j;var s=document.getElementById('net');s.innerHTML='<option value=\"\">&#x8BF7;&#x9009;&#x62E9;&#x7F51;&#x7EDC;</option>';j.forEach(function(n,i){var o=document.createElement('option');o.value=i;o.textContent=(n.ssid||'(\u9690\u85cf\u7f51\u7edc)')+' '+n.rssi+' dBm '+n.auth_name;s.appendChild(o)});st.textContent='\u627e\u5230 '+j.length+' \u4e2a\u7f51\u7edc'}).catch(function(){st.textContent='\u626b\u63cf\u5931\u8d25\uff0c\u8bf7\u91cd\u8bd5'}).then(function(){b.disabled=false})}"
"function pickNetwork(){var i=document.getElementById('net').value;if(i==='')return;var n=nets[Number(i)];document.getElementById('ssid').value=n.ssid;document.getElementById('auth').value=n.auth}"
"function connectWifi(){var b=document.getElementById('connectBtn'),d='ssid='+encodeURIComponent(document.getElementById('ssid').value)+'&password='+encodeURIComponent(document.getElementById('password').value)+'&auth='+encodeURIComponent(document.getElementById('auth').value)+'&token='+encodeURIComponent(document.getElementById('token').value)+'&monitor_ip='+encodeURIComponent(document.getElementById('monitorIp').value.trim());b.disabled=true;st.textContent='\u6b63\u5728\u6d4b\u8bd5\u8fde\u63a5...';fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d}).then(function(r){return r.json().then(function(j){if(!r.ok)throw Error();return j})}).then(function(){st.textContent='\u914d\u7f6e\u5df2\u63d0\u4ea4\uff0c\u6b63\u5728\u8fde\u63a5...';pollStatus()}).catch(function(){st.textContent='\u63d0\u4ea4\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5 Wi-Fi\u3001Token \u548c\u76d1\u63a7 IP';b.disabled=false})}"
"function pollStatus(){fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){var b=document.getElementById('connectBtn');if(j.state===2){st.textContent='\u8fde\u63a5\u6210\u529f\uff0c\u914d\u7f6e\u5df2\u4fdd\u5b58';setTimeout(function(){window.close()},300);return}if(j.state===4){st.textContent='\u8fde\u63a5\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u5bc6\u7801\u548c\u52a0\u5bc6\u65b9\u5f0f\u540e\u91cd\u8bd5';b.disabled=false;return}st.textContent='\u6b63\u5728\u8fde\u63a5\u76ee\u6807 Wi-Fi...';setTimeout(pollStatus,1000)}).catch(function(){st.textContent='\u8bbe\u5907\u5df2\u5207\u6362\u5230\u76ee\u6807 Wi-Fi';document.getElementById('connectBtn').disabled=false})}"
"document.getElementById('scanBtn').addEventListener('click',scanNetworks);document.getElementById('net').addEventListener('change',pickNetwork);document.getElementById('connectBtn').addEventListener('click',connectWifi);</script></main></body></html>";

static void json_escape(const char *input, char *output, size_t size)
{
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 2 < size; ++i) {
        unsigned char c = input[i];
        if (c == '"' || c == '\\') { output[j++] = '\\'; output[j++] = c; }
        else if (c >= 0x20) output[j++] = c;
    }
    output[j] = 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *text)
{
    char *src = text, *dst = text;
    while (*src) {
        if (*src == '+') { *dst++ = ' '; src++; }
        else if (*src == '%' && src[1] && src[2] && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
            *dst++ = (char)((hex_value(src[1]) << 4) | hex_value(src[2])); src += 3;
        } else *dst++ = *src++;
    }
    *dst = 0;
}

static bool form_value(char *body, const char *key, char *output, size_t output_size)
{
    size_t key_len = strlen(key);
    char *part = body;
    while (part && *part) {
        char *next = strchr(part, '&');
        if (next) *next = 0;
        if (!strncmp(part, key, key_len) && part[key_len] == '=') {
            strlcpy(output, part + key_len + 1, output_size);
            url_decode(output);
            if (next) *next = '&';
            return true;
        }
        if (next) { *next = '&'; part = next + 1; } else break;
    }
    return false;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE_SAFE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Received Wi-Fi scan request");
    wifi_manager_ap_t *records = NULL;
    uint16_t count = 0;
    esp_err_t err = wifi_manager_scan(&records, &count);
    if (err != ESP_OK) {
        char response[160];
        snprintf(response, sizeof(response), "{\"message\":\"Wi-Fi scan failed: %s (0x%x)\"}",
                 esp_err_to_name(err), err);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_json(req, response);
    }
    ESP_LOGI(TAG, "Wi-Fi scan returned %u network(s)", count);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "[", 1);
    for (uint16_t i = 0; i < count; ++i) {
        char ssid[96], item[256];
        json_escape(records[i].ssid, ssid, sizeof(ssid));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"auth\":%d,\"auth_name\":\"%s\"}",
                 i ? "," : "", ssid, records[i].rssi, records[i].channel,
                 records[i].authmode, wifi_manager_auth_name(records[i].authmode));
        httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN);
    }
    wifi_manager_free_scan(records);
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"message\":\"Invalid request size\"}");
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) return httpd_resp_send_500(req);
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) { free(body); return ESP_FAIL; }
        received += n;
    }
    char ssid[33] = {0}, password[65] = {0}, auth_text[8] = {0};
    char token[129] = {0}, monitor_ip[16] = {0};
    bool valid = form_value(body, "ssid", ssid, sizeof(ssid)) &&
                 form_value(body, "password", password, sizeof(password)) &&
                 form_value(body, "auth", auth_text, sizeof(auth_text)) &&
                 form_value(body, "token", token, sizeof(token)) &&
                 form_value(body, "monitor_ip", monitor_ip, sizeof(monitor_ip));
    free(body);
    int auth = atoi(auth_text);
    bool supported_auth = auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WEP ||
                          auth == WIFI_AUTH_WPA_PSK || auth == WIFI_AUTH_WPA2_PSK ||
                          auth == WIFI_AUTH_WPA_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK ||
                          auth == WIFI_AUTH_WPA2_WPA3_PSK;
    if (!valid || !supported_auth) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"message\":\"SSID, password or authentication mode is invalid\"}");
    }
    esp_err_t err = wifi_manager_submit_credentials(ssid, password,
                                                    (wifi_auth_mode_t)auth,
                                                    token, monitor_ip);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"message\":\"Configuration rejected; check SSID, encryption and password length\"}");
    }
    return send_json(req, "{\"message\":\"Configuration accepted; testing connection…\"}");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char status[128], escaped[260], response[320];
    wifi_manager_get_status(status, sizeof(status));
    json_escape(status, escaped, sizeof(escaped));
    snprintf(response, sizeof(response), "{\"state\":%d,\"message\":\"%s\"}",
             wifi_manager_get_state(), escaped);
    return send_json(req, response);
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive portal probe: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t head_redirect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive portal HEAD probe: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static void dns_server_task(void *arg)
{
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) { ESP_LOGE(TAG, "DNS socket failed: %d", errno); s_dns_task = NULL; vTaskDelete(NULL); }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s_dns_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno); close(s_dns_socket); s_dns_socket = -1; s_dns_task = NULL; vTaskDelete(NULL);
    }
    uint8_t request[512], response[512];
    while (true) {
        struct sockaddr_in client; socklen_t client_len = sizeof(client);
        int len = recvfrom(s_dns_socket, request, sizeof(request), 0, (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;
        int question_end = 12;
        while (question_end < len && request[question_end]) question_end += request[question_end] + 1;
        question_end += 5;
        if (question_end > len || question_end + 16 > (int)sizeof(response)) continue;
        memcpy(response, request, question_end);
        response[2] = 0x81; response[3] = 0x80;
        response[6] = 0; response[7] = 1;
        int p = question_end;
        response[p++] = 0xC0; response[p++] = 0x0C;
        response[p++] = 0; response[p++] = 1; response[p++] = 0; response[p++] = 1;
        response[p++] = 0; response[p++] = 0; response[p++] = 0; response[p++] = 30;
        response[p++] = 0; response[p++] = 4;
        response[p++] = 192; response[p++] = 168; response[p++] = 4; response[p++] = 1;
        sendto(s_dns_socket, response, p, 0, (struct sockaddr *)&client, client_len);
    }
}

esp_err_t provisioning_portal_start(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 8;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/*", .method = HTTP_GET, .handler = redirect_handler },
        { .uri = "/*", .method = HTTP_HEAD, .handler = head_redirect_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    if (!s_dns_task) xTaskCreate(dns_server_task, "captive_dns", 3072, NULL, 3, &s_dns_task);
    return ESP_OK;
}

void provisioning_portal_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    if (s_dns_socket >= 0) { shutdown(s_dns_socket, SHUT_RDWR); close(s_dns_socket); s_dns_socket = -1; }
    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
}

bool provisioning_portal_is_running(void) { return s_server != NULL; }
