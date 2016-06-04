#include "ets_sys.h"
#include "osapi.h"
#include "driver/uart.h"

#include "user_interface.h"
#include "user_config.h"

//#define USE_UART1_TXD

#define DBG_PRINTF(...)     os_printf(__VA_ARGS__)
#define DBG_FUNCNAME()      do {\
    DBG_PRINTF("[[ %s() ]]\n", __func__);       \
} while (0)

#define HOST_URL    FIREBASE_PRJ ".firebaseio.com"
#define POST_PARAM  "/rest/saving-data/" FIREBASE_SAVE ".json?auth=" FIREBASE_AUTH

static const char M_SSID[] = MY_SSID;
static const char M_PASSWD[] = MY_PASSWD;

#include "espconn.h"

static uint8_t mBuffer[1024];
static uint8_t mBody[256];
static struct espconn mConn;
static esp_tcp mTcp;
static void ICACHE_FLASH_ATTR wifi_eventcb(System_Event_t *evt);
static void ICACHE_FLASH_ATTR dns_done(const char *pName, ip_addr_t *pIpAddr, void *pArg);
static void ICACHE_FLASH_ATTR tcp_connected(void *pArg);
static void ICACHE_FLASH_ATTR tcp_disconnected(void *pArg);
static void ICACHE_FLASH_ATTR data_sent(void *pArg);
static void ICACHE_FLASH_ATTR data_received(void *pArg, char *pData, unsigned short Len);


////////////////////////////////////////////////////////////////
// public
////////////////////////////////////////////////////////////////

#ifdef USE_UART1_TXD
/******************************************************************************
 * FunctionName : uart1_tx_one_char
 * Description  : Internal used function
 *                Use uart1 interface to transfer one char
 * Parameters   : uint8 TxChar - character to tx
 * Returns      : OK
*******************************************************************************/
static void ICACHE_FLASH_ATTR uart1_tx_one_char(uint8 TxChar)
{
    while (READ_PERI_REG(UART_STATUS(UART1)) & (UART_TXFIFO_CNT << UART_TXFIFO_CNT_S)) {
    }

    WRITE_PERI_REG(UART_FIFO(UART1) , TxChar);
}

/******************************************************************************
 * FunctionName : uart1_write_char
 * Description  : Internal used function
 *                Do some special deal while tx char is '\r' or '\n'
 * Parameters   : char c - character to tx
 * Returns      : NONE
*******************************************************************************/
static void ICACHE_FLASH_ATTR uart1_write_char(char c)
{
    if (c == '\n') {
        uart1_tx_one_char('\r');
        uart1_tx_one_char('\n');
    } else if (c == '\r') {
    } else {
        uart1_tx_one_char(c);
    }
}
#endif  //USE_UART1_TXD


void user_rf_pre_init(void)
{
    //system log output : OFF
    //これをすると、自分のos_printfも止まってしまう。
//    system_set_os_print(0);
}


void user_init(void)
{
    //user_rf_pre_init()でuart_init()を呼び出していても、ここでも呼ばないと化けた
    //しかし、system_set_os_print(0)は有効なままだった。
    uart_init(BIT_RATE_115200, BIT_RATE_115200);

#ifdef USE_UART1_TXD
    //出力をUART1に変更
    os_install_putc1((void *)uart1_write_char);
#endif  //USE_UART1_TXD

    DBG_PRINTF("\nReady.\n");

    //リセット要因はuser_init()にならないと取得できない
    struct rst_info *pInfo = system_get_rst_info();
    switch (pInfo->reason) {
    case REASON_DEFAULT_RST:
        DBG_PRINTF("  REASON_DEFAULT_RST\n");
        break;
    case REASON_WDT_RST:
        DBG_PRINTF("  REASON_WDT_RST\n");
        break;
    case REASON_EXCEPTION_RST:
        DBG_PRINTF("  REASON_EXCEPTION_RST\n");
        break;
    case REASON_SOFT_WDT_RST:
        DBG_PRINTF("  REASON_SOFT_WDT_RST\n");
        break;
    case REASON_SOFT_RESTART:
        DBG_PRINTF("  REASON_SOFT_RESTART\n");
        break;
    case REASON_DEEP_SLEEP_AWAKE:
        DBG_PRINTF("  REASON_DEEP_SLEEP_AWAKE\n");
        break;
    case REASON_EXT_SYS_RST:
        DBG_PRINTF("  REASON_EXT_SYS_RST\n");
        break;
    default:
        DBG_PRINTF("  unknown reason : %x\n", pInfo->reason);
        break;
    }

    //////////////////////////////////////////////////////
    struct station_config stconf;
    
    os_memcpy(stconf.ssid, M_SSID, sizeof(M_SSID));
    os_memcpy(stconf.password, M_PASSWD, sizeof(M_PASSWD));
    stconf.bssid_set = 0;
    
    wifi_set_opmode(STATION_MODE);
    //wifi_set_opmode(STATIONAP_MODE);
    wifi_station_set_config(&stconf);
    
    //////////////////////////////////////////////////////

    //タスク
//    system_os_task(event_handler, M_PRIOR, queue, M_SZ_QUEUE);
//    system_os_post(M_PRIOR, 0, 0);

    //WiFiイベント
    wifi_set_event_handler_cb(wifi_eventcb);
}


////////////////////////////////////////////////////////////////
// private
////////////////////////////////////////////////////////////////


//static void ICACHE_FLASH_ATTR event_handler(os_event_t *pEvent)
//{
//    DBG_PRINTF("event!\n");
//}


static void ICACHE_FLASH_ATTR wifi_eventcb(System_Event_t *evt)
{
    err_t err;

    DBG_FUNCNAME();

    DBG_PRINTF("evt->event : %x\n", evt->event);

    switch (evt->event) {
    case EVENT_STAMODE_CONNECTED:
        DBG_PRINTF("[CONN] SSID[%s] CH[%d]\n", evt->event_info.connected.ssid, evt->event_info.connected.channel);
        break;
    case EVENT_STAMODE_DISCONNECTED:
        DBG_PRINTF("[DISC] SSID[%s] REASON[%d]\n", evt->event_info.disconnected.ssid, evt->event_info.disconnected.reason);
        break;
    case EVENT_STAMODE_AUTHMODE_CHANGE:
        DBG_PRINTF("[CHG_AUTH]\n");
        break;
    case EVENT_STAMODE_GOT_IP:
        DBG_PRINTF("[GOT_IP] IP[" IPSTR "] MASK[" IPSTR "] GW[" IPSTR "]\n",
                        IP2STR(&evt->event_info.got_ip.ip),
                        IP2STR(&evt->event_info.got_ip.mask),
                        IP2STR(&evt->event_info.got_ip.gw));
        
        //接続
        {
            ip_addr_t host_ip;
            err = espconn_gethostbyname(&mConn, HOST_URL, &host_ip, dns_done);
            if (err == ESPCONN_OK) {
                //キャッシュにある
                dns_done(HOST_URL, &host_ip, &mConn);
            }
            else if (err == ESPCONN_INPROGRESS) {
                //キャッシュにないので取得しに行く
            }
            else {
                DBG_PRINTF("espconn_gethostbyname fail : %d\n", err);
            }
        }
        break;
    case EVENT_STAMODE_DHCP_TIMEOUT:
        break;
    default:
        break;
    }
}


static void ICACHE_FLASH_ATTR dns_done(const char *pName, ip_addr_t *pIpAddr, void *pArg)
{
    struct espconn *pConn = (struct espconn *)pArg;

    DBG_FUNCNAME();

    if (pConn == NULL) {
        DBG_PRINTF("DNS lookup fail.\n");
        return;
    }

    pConn->type = ESPCONN_TCP;
    pConn->state = ESPCONN_NONE;
    pConn->proto.tcp = &mTcp;
    pConn->proto.tcp->local_port = espconn_port();
    pConn->proto.tcp->remote_port = 443;         //https
    os_memcpy(pConn->proto.tcp->remote_ip, &pIpAddr->addr, 4);
    
    espconn_regist_connectcb(pConn, tcp_connected);
    espconn_regist_disconcb(pConn, tcp_disconnected);
    espconn_secure_set_size(ESPCONN_CLIENT, 1024*5);      //SSL
    espconn_secure_connect(pConn);                      //SSL
}


static void ICACHE_FLASH_ATTR tcp_connected(void *pArg)
{
    err_t err;
    struct espconn *pConn = (struct espconn *)pArg;

    DBG_FUNCNAME();

    espconn_regist_recvcb(pConn, data_received);
    espconn_regist_sentcb(pConn, data_sent);

    os_sprintf(mBody,
            "{\"test1\":{\"now\":{\".sv\":\"timestamp\"},\"name\":\"hiro99ma\",\"date\":\"06/04\"}}\r\n"
    );

    os_sprintf(mBuffer,
            "POST " POST_PARAM " HTTP/1.1\r\n"
            "Host: " HOST_URL "\r\n"
            "Accept: */*\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s", os_strlen(mBody), mBody);
    DBG_PRINTF("len=%d\n", os_strlen(mBuffer));
    DBG_PRINTF("[%s]\n", mBuffer);
    err = espconn_secure_send(pConn, mBuffer, os_strlen(mBuffer));      //SSL
    if (err != 0) {
        DBG_PRINTF("espconn_secure_send fail: %d\n", err);
    }
}


static void ICACHE_FLASH_ATTR tcp_disconnected(void *pArg)
{
    struct espconn *pConn = (struct espconn *)pArg;

    DBG_FUNCNAME();

    wifi_station_disconnect();
}


static void ICACHE_FLASH_ATTR data_sent(void *pArg)
{
    DBG_FUNCNAME();
}


static void ICACHE_FLASH_ATTR data_received(void *pArg, char *pData, unsigned short Len)
{
    struct espconn *pConn = (struct espconn *)pArg;

    DBG_FUNCNAME();

    DBG_PRINTF("----------\n");
    DBG_PRINTF(pData);
    DBG_PRINTF("\n----------\n");

    espconn_secure_disconnect(pConn);           //SSL
}
