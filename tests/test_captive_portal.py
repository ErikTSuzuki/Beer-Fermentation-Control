"""Test the real captive DNS parser and FreeRTOS/DHCP/socket adapter on host.

No sockets are opened: network and RTOS calls in the adapter are mocked.
Run: python3 tests/test_captive_portal.py [path-to-gcc]
"""
from pathlib import Path
import shutil
import struct
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/captive-tests"

CORE = r'''
#include "CaptiveDns.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const uint8_t ip[4]={192,168,50,1};
int main(int argc, char **argv) {
    (void)argv;
    if(argc>1) {
        uint32_t random=123456;
        for(unsigned iteration=0;iteration<20000;++iteration) {
            uint8_t query[513], before[514], output[514];
            for(size_t i=0;i<sizeof(query);++i){random=random*1664525u+1013904223u;query[i]=(uint8_t)(random>>24);}
            size_t length=random%514;
            if(iteration%2==0 && length>=17){memset(query,0,12);query[2]=1;query[5]=1;}
            memset(output,0x55,sizeof(output));memcpy(before,output,sizeof(output));
            size_t capacity=random%513;
            size_t size=captive_dns_reply(query,length,ip,output+1,capacity);
            assert(output[0]==0x55 && output[capacity+1]==0x55);
            if(!size)assert(!memcmp(before,output,sizeof(output)));
            else{assert(size<=capacity && size>=17);assert(output[3]&0x80);assert(output[6]==1);}
        }
        puts("20000 captive DNS randomized bounds checks passed");return 0;
    }
    uint8_t query[513],response[512];
    size_t length=fread(query,1,sizeof(query),stdin);
    size_t size=captive_dns_reply(query,length,ip,response,sizeof(response));
    if(size) fwrite(response,1,size,stdout);
}
'''

MOCK = r'''
#ifndef CAPTIVE_MOCK_H
#define CAPTIVE_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
typedef int esp_err_t;
typedef int esp_netif_t;
typedef uint32_t TickType_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { struct { int type; union { esp_ip4_addr_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;
typedef uint8_t dhcps_offer_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_OP_SET 1
#define ESP_NETIF_DOMAIN_NAME_SERVER 6
#define OFFER_DNS 2
#define CONFIG_BEER_AP_IP "192.168.50.1"
#define pdPASS 1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define ESP_LOGI(tag, ...) mock_log(tag,__VA_ARGS__)
void mock_log(const char *,const char *,...);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *);
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *,char *);
esp_err_t esp_netif_set_dns_info(esp_netif_t *,int,esp_netif_dns_info_t *);
esp_err_t esp_netif_dhcps_option(esp_netif_t *,int,int,void *,size_t);
esp_err_t esp_netif_str_to_ip4(const char *,esp_ip4_addr_t *);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
int mock_socket(int,int,int);
int mock_setsockopt(int,int,int,const void *,socklen_t);
int mock_bind(int,const struct sockaddr *,socklen_t);
int mock_close(int);
ssize_t mock_recvfrom(int,void *,size_t,int,struct sockaddr *,socklen_t *);
ssize_t mock_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
#define socket mock_socket
#define setsockopt mock_setsockopt
#define bind mock_bind
#define close mock_close
#define recvfrom mock_recvfrom
#define sendto mock_sendto
#endif
'''

ADAPTER = r'''
#include "CaptivePortal.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { assert(x); ++checks; } while (0)
static unsigned checks,dnsWrites,offers,opens,closes,sends,stage;
static esp_netif_t ap=1,sta=2;
static void (*task)(void *);
static void *taskContext;
static const char *mode;
static jmp_buf finish;
static const uint8_t request[]={0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
void mock_log(const char *tag,const char *format,...){(void)tag;(void)format;}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key){CHECK(!strcmp(key,"WIFI_AP_DEF"));return !strcmp(mode,"missing-ap")?NULL:&ap;}
int esp_netif_get_ip_info(esp_netif_t *netif,esp_netif_ip_info_t *out){
    CHECK(netif==&ap);if(!strcmp(mode,"ip-fail"))return ESP_FAIL;out->ip.addr=inet_addr(CONFIG_BEER_AP_IP);return ESP_OK;
}
int esp_netif_get_netif_impl_name(esp_netif_t *netif,char *out){
    CHECK(netif==&ap);if(!strcmp(mode,"interface-fail"))return ESP_FAIL;strcpy(out,"ap1");return ESP_OK;
}
int esp_netif_str_to_ip4(const char *address,esp_ip4_addr_t *out){out->addr=inet_addr(address);return ESP_OK;}
int esp_netif_set_dns_info(esp_netif_t *netif,int kind,esp_netif_dns_info_t *dns){
    CHECK(netif==&ap && kind==ESP_NETIF_DNS_MAIN && dns->ip.u_addr.ip4.addr==inet_addr(CONFIG_BEER_AP_IP));
    ++dnsWrites;return !strcmp(mode,"dns-fail")?ESP_FAIL:ESP_OK;
}
int esp_netif_dhcps_option(esp_netif_t *netif,int op,int option,void *value,size_t size){
    CHECK(netif==&ap && op==ESP_NETIF_OP_SET && option==ESP_NETIF_DOMAIN_NAME_SERVER);
    CHECK(size==sizeof(dhcps_offer_t) && *(dhcps_offer_t *)value==OFFER_DNS);++offers;
    return !strcmp(mode,"offer-fail")?ESP_FAIL:ESP_OK;
}
int mock_socket(int family,int type,int protocol){
    CHECK(family==AF_INET && type==SOCK_DGRAM && protocol==IPPROTO_UDP);++opens;
    return !strcmp(mode,"socket-fail")?-1:6+(int)opens;
}
int mock_setsockopt(int fd,int level,int option,const void *value,socklen_t size){
    CHECK(fd>=7 && level==SOL_SOCKET);
    if(option==SO_BINDTODEVICE){CHECK(size==sizeof(struct ifreq));CHECK(!strcmp(((const struct ifreq *)value)->ifr_name,"ap1"));}
    else{CHECK(option==SO_RCVTIMEO && size==sizeof(struct timeval));CHECK(((const struct timeval *)value)->tv_sec==1);}
    return !strcmp(mode,"option-fail")?-1:0;
}
int mock_bind(int fd,const struct sockaddr *address,socklen_t size){
    const struct sockaddr_in *ipv4=(const struct sockaddr_in *)address;
    CHECK(fd>=7 && size==sizeof(*ipv4) && ipv4->sin_family==AF_INET);
    CHECK(ipv4->sin_addr.s_addr==inet_addr(CONFIG_BEER_AP_IP) && ipv4->sin_port==htons(53));
    return !strcmp(mode,"bind-fail")?-1:0;
}
int mock_close(int fd){CHECK(fd>=7);++closes;return 0;}
ssize_t mock_recvfrom(int fd,void *bytes,size_t capacity,int flags,struct sockaddr *peer,socklen_t *size){
    CHECK(fd>=7 && capacity==513 && flags==0 && *size==sizeof(struct sockaddr_in));
    struct sockaddr_in *ipv4=(struct sockaddr_in *)peer;
    *ipv4=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(53000),.sin_addr={.s_addr=inet_addr("192.168.50.2")}};
    memcpy(bytes,request,sizeof(request));
    switch(stage++){
        case 0: return sizeof(request);
        case 1: ipv4->sin_family=AF_INET6;return sizeof(request);
        case 2: ipv4->sin_port=0;return sizeof(request);
        case 3: memset(bytes,0,capacity);return capacity; // oversized/truncated datagram
        case 4: return 3;
        case 5: errno=EAGAIN;return -1;
        case 6: errno=EINTR;return -1;
        case 7: errno=EBADF;return -1;
        case 8: return sizeof(request);
        default: assert(false);return -1;
    }
}
ssize_t mock_sendto(int fd,const void *bytes,size_t size,int flags,const struct sockaddr *peer,socklen_t length){
    const uint8_t *answer=bytes;const struct sockaddr_in *ipv4=(const struct sockaddr_in *)peer;
    const uint8_t ip[]={192,168,50,1};
    CHECK(fd>=7 && flags==0 && size==sizeof(request)+16 && length==sizeof(*ipv4));
    CHECK(ipv4->sin_port==htons(53000) && answer[0]==0x12 && answer[1]==0x34 && answer[7]==1);
    CHECK(!memcmp(answer+size-4,ip,4));++sends;return size;
}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *context,unsigned priority,void *handle){
    CHECK(!strcmp(name,"captive_dns") && stack==4096 && priority==1 && handle==NULL);
    if(!strcmp(mode,"task-fail"))return 0;
    task=fn;taskContext=context;return pdPASS;
}
void vTaskDelete(void *context){(void)context;assert(false);}
void vTaskDelay(TickType_t delay){
    CHECK(delay==10 || delay==1000);
    if(sends==2){CHECK(opens==2 && closes==1);longjmp(finish,1);}
}
int main(int argc,char **argv){
    CHECK(argc==2);mode=argv[1];
#if CONFIG_BEER_CAPTIVE_PORTAL_ENABLED
    CHECK(captive_portal_configure_dhcp(&sta)==ESP_ERR_INVALID_ARG && dnsWrites==0);
    esp_err_t configured=captive_portal_configure_dhcp(&ap);
    if(!strcmp(mode,"missing-ap")){CHECK(configured==ESP_ERR_INVALID_ARG);}
    else if(!strcmp(mode,"dns-fail")){CHECK(configured==ESP_FAIL && dnsWrites==1 && offers==0);}
    else if(!strcmp(mode,"offer-fail")){CHECK(configured==ESP_FAIL && dnsWrites==1 && offers==1);}
    else if(!strcmp(mode,"ip-fail")){CHECK(configured==ESP_FAIL && dnsWrites==0);}
    else CHECK(configured==ESP_OK && dnsWrites==1 && offers==1);
    esp_err_t result=captive_portal_start();
    if(!strcmp(mode,"socket-fail") || !strcmp(mode,"missing-ap") || !strcmp(mode,"ip-fail") || !strcmp(mode,"interface-fail")){
        CHECK(result==ESP_FAIL && closes==0 && !task);
    }else if(!strcmp(mode,"bind-fail") || !strcmp(mode,"option-fail")){
        CHECK(result==ESP_FAIL && closes==1 && !task);
    }else if(!strcmp(mode,"task-fail")){
        CHECK(result==ESP_ERR_NO_MEM && closes==1 && !task);
    }else{
        CHECK(result==ESP_OK && task);CHECK(captive_portal_start()==ESP_ERR_INVALID_STATE);
        if(!setjmp(finish))task(taskContext);
    }
#else
    CHECK(captive_portal_configure_dhcp(&ap)==ESP_OK && captive_portal_start()==ESP_OK);
    CHECK(captive_portal_configure_dhcp(&sta)==ESP_OK);
    CHECK(opens==0 && dnsWrites==0 && !task);
#endif
    printf("%u captive RTOS/DHCP/socket checks passed (%s)\n",checks,mode);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("gcc required")
    BUILD.mkdir(parents=True, exist_ok=True)
    core = BUILD / "dns_test.c"
    core.write_text(CORE, encoding="utf-8")
    dns = BUILD / "dns_test"
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-D_DEFAULT_SOURCE",
              "-I" + str(BUILD), "-I" + str(PROJECT / "main")]
    subprocess.run([*common, str(core), str(PROJECT / "main/CaptiveDns.c"), "-o", str(dns)], check=True)
    subprocess.run([str(dns), "fuzz"], check=True, timeout=10)

    def query(name="connectivitycheck.gstatic.com", kind=1, qclass=1, flags=0x100):
        encoded = b"".join(bytes([len(label)]) + label.encode() for label in name.split(".")) + b"\0"
        return struct.pack("!6H", 0x1234, flags, 1, 0, 0, 0) + encoded + struct.pack("!HH", kind, qclass)

    def reply(packet):
        return subprocess.run([str(dns)], input=packet, capture_output=True, check=True, timeout=5).stdout

    count = 0
    for name in ("connectivitycheck.gstatic.com", "captive.apple.com", "www.msftconnecttest.com", "example.com",
                 ".".join(["a" * 63] * 3 + ["b" * 61])):
        for kind in (1, 28, 64, 65, 255):
            packet = query(name, kind)
            result = reply(packet)
            header = struct.unpack("!6H", result[:12])
            assert header == (0x1234, 0x8500, 1, int(kind == 1), 0, 0)
            assert result[12:len(packet)] == packet[12:]
            if kind == 1:
                assert result[len(packet):] == struct.pack("!HHHIH4B", 0xc00c, 1, 1, 0, 4, 192, 168, 50, 1)
            else:
                assert len(result) == len(packet)
            count += 1
    packet = query()
    for length in range(len(packet)):
        assert reply(packet[:length]) == b""
        count += 1
    for start, value in ((2, 0x8000), (2, 0x7800), (2, 0x200), (4, 0), (4, 2), (6, 1), (8, 1)):
        invalid = bytearray(packet); invalid[start:start + 2] = struct.pack("!H", value)
        assert not reply(invalid)
        count += 1
    for invalid in (packet + b"\0" * 512,
                    struct.pack("!6H", 1, 0x100, 1, 0, 0, 0) + b"\xc0\x0c\0\1\0\1",
                    query(".".join(["a" * 63] * 3 + ["b" * 62])),
                    query("a" * 64)):
        assert not reply(invalid)
        count += 1
    # EDNS does not inflate answer counts or get copied into the response.
    edns = bytearray(packet); edns[11] = 1
    assert reply(edns + b"\0\0\x29\x04\xd0\0\0\0\0\0\0") == reply(packet)
    assert struct.unpack("!H", reply(query(qclass=3))[6:8])[0] == 0
    print(f"{count + 2} captive DNS protocol scenarios passed")
    (BUILD / "captive_mock.h").write_text(MOCK, encoding="utf-8")
    for name in ("esp_err.h", "esp_netif.h", "sdkconfig.h", "esp_log.h", "freertos/FreeRTOS.h",
                 "freertos/task.h", "lwip/inet.h", "lwip/sockets.h", "dhcpserver/dhcpserver.h"):
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "captive_mock.h"\n', encoding="utf-8")
    source = BUILD / "adapter_test.c"
    source.write_text(ADAPTER, encoding="utf-8")
    for enabled in (1, 0):
        executable = BUILD / f"adapter_test_{enabled}"
        subprocess.run([*common, f"-DCONFIG_BEER_CAPTIVE_PORTAL_ENABLED={enabled}", str(source),
            str(PROJECT / "main/CaptiveDns.c"), "-o", str(executable)], check=True)
        modes = ("normal", "dns-fail", "offer-fail", "missing-ap", "ip-fail", "interface-fail",
                 "socket-fail", "option-fail", "bind-fail", "task-fail") if enabled else ("disabled",)
        for mode in modes:
            subprocess.run([str(executable), mode], check=True, timeout=10)


if __name__ == "__main__":
    main()
