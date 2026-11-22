// Для strlen и memset.
#include <string.h>
// Семафоры K_SEM_DEFINE sem_wifi и sem_ipv4.
#include <zephyr/kernel.h>
// Управление Wi‑Fi через Net Management API. Предоставляет структуры (wifi_connect_req_params, wifi_iface_status, wifi_status), типы безопасности (WIFI_SECURITY_TYPE_PSK) и коды событий (NET_EVENT_WIFI_CONNECT_RESULT и др.).
#include <zephyr/net/wifi_mgmt.h>

// Event callbacks – структура, регистрируемая в подсистеме net_mgmt для получения уведомлений о событиях.
// Wi-Fi connection request завершился успешным подключением.
static struct net_mgmt_event_callback wifi_cb;
// Интерфейс получил IPv4-адрес.
static struct net_mgmt_event_callback ipv4_cb;

// Semaphores
static K_SEM_DEFINE(sem_wifi, 0, 1);
static K_SEM_DEFINE(sem_ipv4, 0, 1);

// Called when the WiFi is connected
// Получить от Zephyr событие изменения состояния Wi-Fi, определить, 
// что произошло, проверить результат операции и синхронизировать основной поток через sem_wifi.
// struct net_mgmt_event_callback *cb - Указатель на структуру callback-а net_mgmt. Объект был зарегистрирован ранее как wifi_cb.
// uint64_t mgmt_event - Идентификатор произошедшего сетевого management event (NET_EVENT_WIFI_CONNECT_RESULT и NET_EVENT_WIFI_DISCONNECT_RESULT).
// struct net_if *iface - Указатель на сетевой интерфейс Wi-Fi ESP3
static void on_wifi_connection_event(struct net_mgmt_event_callback *cb,
                                    uint64_t mgmt_event,
                                    struct net_if *iface) {
    // Указатель на структуру wifi_status для событий Wi‑Fi Connect/Disconnect.
    const struct wifi_status *status = (const struct wifi_status *)cb->info;

    // Проверяем, является ли полученное событие результатом запроса на подключение к Wi-Fi.
    if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        // Проверяем код результата операции. status->status == 0: операция завершилась успешно.
        // Сигнализируем ожидающему потоку, что Wi-Fi подключён.
        if (status->status) {
            printk("Error (%d): Connection request failed\r\n", status->status);
        } else {
            printk("Connected!\r\n");
            k_sem_give(&sem_wifi);
        }
    // Проверяем, является ли полученное событие результатом запроса на отключение от Wi-Fi.
    } else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        if (status->status) {
            printk("Error (%d): Disconnection request failed\r\n", status->status);
        } else {
            printk("Disconnected\r\n");
            k_sem_take(&sem_wifi, K_NO_WAIT);
        }
    }
}



// Event handler for WiFi management events
// Эта функция является callback-обработчиком сетевого события: NET_EVENT_IPV4_ADDR_ADD
// то есть на событие добавления IPv4-адреса к сетевому интерфейсу.
// struct net_mgmt_event_callback *cb - Указатель на структуру callback-а net_mgmt. Объект был зарегистрирован ранее как wifi_cb.
// uint64_t mgmt_event - Идентификатор произошедшего сетевого management event (NET_EVENT_WIFI_CONNECT_RESULT и NET_EVENT_WIFI_DISCONNECT_RESULT).
// struct net_if *iface - Указатель на сетевой интерфейс Wi-Fi ESP3
static void on_ipv4_obtained(struct net_mgmt_event_callback *cb,
                            uint64_t mgmt_event,
                            struct net_if *iface) {
    // Signal that the IP address has been obtained
    // Проверяем, какое именно событие произошло.
    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        // Сигнализируем семафору, что IPv4-адрес был получен.
        k_sem_give(&sem_ipv4);
    }
}


// Initialize the WiFi event callbacks
// Функция регистрирует обработчики событий:
// 1. on_wifi_connection_event(NET_EVENT_WIFI_CONNECT_RESULT или NET_EVENT_WIFI_DISCONNECT_RESULT)
// 2. on_ipv4_obtained(NET_EVENT_IPV4_ADDR_ADD)
// После выполнения wifi_init() сетевой стек Zephyr знает:
// "Когда произойдёт NET_EVENT_WIFI_CONNECT_RESULT, вызови on_wifi_connection_event()."
// "Когда произойдёт NET_EVENT_WIFI_DISCONNECT_RESULT,вызови on_wifi_connection_event()."
// "Когда произойдёт NET_EVENT_IPV4_ADDR_ADD, вызови on_ipv4_obtained()."
void wifi_init(void) {
    // Initialize the event callbacks
    net_mgmt_init_event_callback(&wifi_cb,
                                on_wifi_connection_event,
                                NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_init_event_callback(&ipv4_cb,
                                on_ipv4_obtained,
                                NET_EVENT_IPV4_ADDR_ADD);
        
    // Add the event callbacks
    net_mgmt_add_event_callback(&wifi_cb);
    net_mgmt_add_event_callback(&ipv4_cb);
}



// Connect to the WiFi network (blocking)
// Функция запускает подключение устройства к Wi-Fi сети и затем ожидает,
// пока callback on_wifi_connection_event() сообщит об успешном подключении.
// char *ssid - Указатель на строку с именем Wi-Fi сети (SSID).
// char *psk - Указатель на строку с паролем Wi-Fi сети.
int wifi_connect(char *ssid, char *psk) {
    // Объявляем локальную переменную ret.
    int ret;
    // Объявляем указатель iface на структуру: struct net_if (Wi-Fi интерфейс).
    struct net_if *iface;
    // Эта структура содержит параметры запроса на подключение к Wi-Fi.
    // В неё мы позже положим: SSID, длину SSID, PSK, длину PSK, ...
    struct wifi_connect_req_params params = {0};
    // Функция возвращает указатель на интерфейс, который Zephyr считает интерфейсом по умолчанию.
    iface = net_if_get_default();

    // Fill in the connection request parameters
    params.ssid = (const uint8_t *)ssid;
    params.ssid_length = strlen(ssid);
    params.psk = (const uint8_t *)psk;
    params.psk_length = strlen(psk);
    params.security = WIFI_SECURITY_TYPE_PSK;
    params.band = WIFI_FREQ_BAND_UNKNOWN;
    params.channel = WIFI_CHANNEL_ANY;
    params.mfp = WIFI_MFP_OPTIONAL;
    params.timeout = SYS_FOREVER_MS;


    // Connect to the WiFi network
    //  NET_REQUEST_WIFI_CONNECT - request code — код запроса. Сигнализирует что нужно выполнить запрос подключения к Wi-Fi.
    // iface - указатель на сетевой интерфейс, который мы получили ранее.
    // &params - структура с параметрами подключения.
    // sizeof(params) - размер структуры params в байтах.
    ret = net_mgmt(NET_REQUEST_WIFI_CONNECT,
                    iface,
                    &params,
                    sizeof(params));

    // Wait for the connection to complete
    // ОЖИДАНИЕ РЕЗУЛЬТАТА WIFI ПОДКЛЮЧЕНИЯ.
    k_sem_take(&sem_wifi, K_FOREVER);

    return ret;
}

// Wait for IP address (blocking)
// Функция блокирует выполнение до тех пор, пока сетевой интерфейс не получит
// IPv4-адрес (событие NET_EVENT_IPV4_ADDR_ADD), затем запрашивает у Wi-Fi
// драйвера текущее состояние соединения и информацию об адресах. Вся полученная информация выводится в консоль.
void wifi_wait_for_ip_addr(void) {
    // Переменная для хранения полного статуса Wi-Fi интерфейса.
    struct wifi_iface_status status;
    // Указатель на сетевой интерфейс, с которым будем работать.
    struct net_if *iface;
    // Буферы для хранения IP-адреса и адреса шлюза в виде строк.
    char ip_addr[NET_IPV4_ADDR_LEN];
    char gw_addr[NET_IPV4_ADDR_LEN];

    // Get interface
    // Получаем указатель на сетевой интерфейс по умолчанию.
    iface = net_if_get_default();

    // Wait for the IPv4 address to be obtained
    // Блокирующее ожидание семафора sem_ipv4 гарантирует, что дальнейший код выполняется только после получения IP-адреса.
    k_sem_take(&sem_ipv4, K_FOREVER);

    // Get the WiFi status
    // Запрашиваем текущее состояние Wi-Fi интерфейса через &status.
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS,
                iface,
                &status,
                sizeof(struct wifi_iface_status))) {
        printk("Error: WiFi status request failed\r\n");
    }

    // Get the IP address
    // Подготавливаем буфер ip_addr, заполняя его нулями.
    memset(ip_addr, 0, sizeof(ip_addr));
    // Преобразуем IP-адрес интерфейса из бинарного представления в строку.
    if (net_addr_ntop(AF_INET,
                    &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
                    ip_addr,
                    sizeof(ip_addr)) == NULL) {
        printk("Error: Could not convert IP address to string\r\n");
    }

    // Get the gateway address
    memset(gw_addr, 0, sizeof(gw_addr));
    if (net_addr_ntop(AF_INET,
                    &iface->config.ip.ipv4->gw,
                    gw_addr,
                    sizeof(gw_addr)) == NULL) {
        printk("Error: Could not convert gateway address to string\r\n");
    }

    // Print the WiFi status
    printk("WiFi status:\r\n");
    if (status.state >= WIFI_STATE_ASSOCIATED) {
        printk("  SSID: %-32s\r\n", status.ssid);
        printk("  Band: %s\r\n", wifi_band_txt(status.band));
        printk("  Channel: %d\r\n", status.channel);
        printk("  Security: %s\r\n", wifi_security_txt(status.security));
        printk("  IP address: %s\r\n", ip_addr);
        printk("  Gateway: %s\r\n", gw_addr);
    }
}


// Disconnect from the WiFi network
// Функция инициирует процесс отключения, вызывая net_mgmt с командой NET_REQUEST_WIFI_DISCONNECT.
// Используется в main.c для явного разрыва Wi-Fi соединения, например, при завершении работы приложения или перед переподключением к другой сети.
int wifi_disconnect(void) {
    // Переменная для сохранения кода возврата net_mgmt.
    int ret;
    // Указатель на сетевой интерфейс, для которого выполняется отключение.
    struct net_if *iface = net_if_get_default();
    // Вызов net_mgmt с запросом на отключение от Wi-Fi сети.
    ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    // Возвращаемое значение: 0 - запрос успешно отправлен в драйвер Wi-Fi. 
    return ret;
}